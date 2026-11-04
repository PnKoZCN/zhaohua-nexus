# 偏移量采集方法

本文档说明 ZhaoHua Nexus 数据表中各项偏移量的采集原理与操作流程。

---

## 一、为什么需要偏移量

`libminecraftpe.so` 是闭合源码的原生库，没有可用的符号调试信息。要 hook 某个函数、读写某个字段，必须知道它在库里的**精确地址偏移**。

引擎不含版本适配逻辑，所有偏移以数据表形式打包（`zh_db_full.bin`），**换版本必须重新采集**。

---

## 二、数据表与采集方式对应

| 表 | 条目数 | 采集目标 | 采集方法 |
|---|---|---|---|
| `rettype` | 124 | 函数返回类型 | 符号表 + 返回指令模式 |
| `retfunc` | 124 | 含返回点的函数名 | 同上 |
| `retpoint` | 2,754 | 每个 `ret` 指令的地址 | 反汇编遍历 |
| `blcall` | 449 | `bl` 调用点 → 被调函数 | 反汇编 + 符号对齐 |
| `const` | 26,447 | 函数内常量值 | 立即数扫描 |
| `constfunc` | 121 | 含常量的函数名 | 同上 |
| `frame` | 120 | 栈帧大小 | 栈指针调整指令分析 |
| `slot` | 547 | 栈槽位偏移与宽度 | `[sp,#off]` 访存扫描 |
| `fieldacc` | 716 | 字段访问（偏移 / 基址 / 宽度） | getter 函数体分析 |
| `poolhint` | 5 | 可用的零填充内存区 | `.bss` / 代码段空隙扫描 |

---

## 三、基础准备

### 3.1 获取目标库

```bash
# 从设备拉取（需 root 或 Shizuku）
adb shell su -c "cp /data/app/*/com.netease.x19*/lib/arm64/libminecraftpe.so /sdcard/"
adb pull /sdcard/libminecraftpe.so
```

**校验版本**：文件大小 `323,630,848` 字节对应 `3.10.5.301327`。

### 3.2 获取符号表

```bash
# 动态符号（导出的 C++ 函数）
readelf --dyn-syms -W libminecraftpe.so > syms.txt

# 或 nm
nm -D --defined-only libminecraftpe.so > syms.txt
```

**输出格式**：
```
000000000677d6ac T _ZN25ContainerScreenController23getStackFromModelEi
```

第三列是**虚拟地址**（不是文件偏移）。对 `.so` 通常可直接作为基址偏移使用，但要确认所在 `PT_LOAD` 段的 `p_vaddr`。

### 3.3 反汇编

```bash
# 需要能处理 arm64 的 binutils
aarch64-linux-gnu-objdump -d libminecraftpe.so > disasm.txt

# 或只用 NDK 自带的
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-objdump -d libminecraftpe.so
```

---

## 四、各表采集细则

### 4.1 `rettype` / `retfunc` / `retpoint` —— 返回点

**目标**：找出函数内所有 `ret` 指令的地址。

**ARM64 `ret` 编码**：
```
0xD65F0000 | (Rn << 5)
  Rn = 30 时为 `ret`（0xD65F03C0）
  Rn = 其它为 `ret xN`（如函数尾指针跳转）
```

**扫描方法**：
```python
import struct

RET_MASK = 0xFFFFFC1F
RET_BASE = 0xD65F0000

def find_ret_points(so_path, func_offset, func_size):
    """扫描函数体内所有 ret 指令"""
    points = []
    for off in range(0, func_size, 4):
        f.seek(func_offset + off)
        w = struct.unpack('<I', f.read(4))[0]
        if (w & RET_MASK) == RET_BASE:
            points.append((func_offset + off, (w >> 5) & 0x1F))
    return points
```

**注意**：
- `ret` 指令地址 `ra`，hook 时用 `ra - 4`（`ret` 前的 4 字节位置）插桩
- 最后一个 `ret` 标记为 `isLast = 1`（用于 `@Inject(at = TAIL)`）

**返回类型推断**：
```python
# 看 ret 之前 x0 是怎么被写的
#   ldr x0, ...      → 指针
#   fmov s0/d0, ...  → float/double
#   mov w0, #0       → int/bool
#   无 x0 写入       → void
```

---

### 4.2 `blcall` —— 方法调用点

**目标**：找出函数内所有 `bl` 指令，以及它们跳向哪个已知函数。

**ARM64 `bl` 编码**：
```
0x94000000 | (imm26 & 0x03FFFFFF)
偏移 = sign_extend(imm26, 26) << 2
目标 = 指令地址 + 偏移
```

**扫描方法**：
```python
BL_MASK = 0xFC000000
BL_BASE = 0x94000000

def find_bl_calls(so_path, func_offset, func_size, sym_map):
    """sym_map: {绝对地址: 函数名}"""
    calls = []
    for off in range(0, func_size, 4):
        addr = func_offset + off
        f.seek(addr)
        w = struct.unpack('<I', f.read(4))[0]
        if (w & BL_MASK) == BL_BASE:
            imm = w & 0x03FFFFFF
            if imm & 0x02000000:
                imm -= 0x04000000
            target = addr + (imm << 2)
            name = sym_map.get(target)
            if name:
                calls.append((addr, target, name))
    return calls
```

**验证**：解析出的目标地址必须精确等于某个已知函数的偏移。

---

### 4.3 `const` / `constfunc` —— 函数内常量

**目标**：找出函数体内编码了特定常量值的指令。

**ARM64 立即数指令**：

| 指令 | 编码 | 用途 |
|---|---|---|
| `movz xN, #imm16, lsl #shift` | `0xD2800000 \| (imm16<<5) \| (shift/16<<21) \| N` | 装载 16 位 |
| `movk xN, #imm16, lsl #shift` | `0xF2800000 \| ...` | 拼接 16 位 |
| `movn xN, #imm16, lsl #shift` | `0x92800000 \| ...` | 装载反码 |
| `cmp xN, #imm12`（subs） | `0xF1000000 \| (imm12<<10) \| ...` | 比较常量 |
| `add xN, xM, #imm12` | `0x91000000 \| (imm12<<10) \| ...` | 加常量 |

**扫描方法**：
```python
def scan_constants(so_path, func_offset, func_size):
    consts = []
    for off in range(0, func_size, 4):
        addr = func_offset + off
        f.seek(addr)
        w = struct.unpack('<I', f.read(4))[0]

        # movz 32 位
        if (w & 0x7F800000) == 0x52800000:
            imm16 = (w >> 5) & 0xFFFF
            shift = ((w >> 21) & 0x3) * 16
            consts.append((addr, imm16 << shift, 'movz', 32))

        # movz 64 位
        elif (w & 0x7F800000) == 0x52C00000:
            imm16 = (w >> 5) & 0xFFFF
            shift = ((w >> 21) & 0x3) * 16
            consts.append((addr, imm16 << shift, 'movz', 64))

        # subs（cmp）立即数
        elif (w & 0x7F000000) == 0x71000000:
            imm12 = (w >> 10) & 0xFFF
            shift = ((w >> 22) & 0x1) * 12
            consts.append((addr, imm12 << shift, 'cmp', 32))
    return consts
```

**过滤噪声**：同一个偏移上，如果某常量值出现超过 3 次，通常是编译器生成的模板代码，应剔除。

---

### 4.4 `frame` / `slot` —— 栈帧与槽位

**目标**：确定函数的栈帧大小，以及每个 `[sp, #off]` 访问的偏移和宽度。

**栈帧大小推断**——找函数开头的 `sp` 调整：
```python
# sub sp, sp, #imm      (0xD10003FF | imm12<<10)  → 32 位
# sub sp, sp, #imm, lsl #12
# stp x29, x30, [sp, #-N]!   (前索引)

def get_frame_size(so_path, func_offset):
    f.seek(func_offset)
    for _ in range(8):                    # 前 8 条指令内
        w = struct.unpack('<I', f.read(4))[0]

        # sub sp, sp, #imm12
        if (w & 0xFFC003FF) == 0xD10003FF:
            imm = (w >> 10) & 0xFFF
            return imm

        # stp x29,x30,[sp,#-N]!  (pre-index)
        if (w & 0x7FC00000) == 0x29800000:
            imm = ((w >> 15) & 0x7F) * 8
            # 继续往后看有没有 sub sp
    return 0
```

**槽位扫描**——扫描函数体内所有 `[sp, #off]` 访存：

```python
def scan_slots(so_path, func_offset, func_size):
    """扫描 [sp, #off] 的存取"""
    slots = []
    for off in range(0, func_size, 4):
        addr = func_offset + off
        f.seek(addr)
        w = struct.unpack('<I', f.read(4))[0]

        # STR/LDR (unsigned offset) 64位: size=11
        #   0xF9000000 | (imm12<<10) | (Rn<<5) | Rt
        if (w & 0xFFC00000) == 0xF9000000:
            rn = (w >> 5) & 0x1F
            if rn == 31:                    # 基址是 sp
                imm12 = (w >> 10) & 0xFFF
                rt = w & 0x1F
                slots.append((addr, imm12 * 8, 8, rt))

        # STR/LDR 32位: size=10
        #   0xB9000000 | ...
        elif (w & 0xFFC00000) == 0xB9000000:
            rn = (w >> 5) & 0x1F
            if rn == 31:
                imm12 = (w >> 10) & 0xFFF
                rt = w & 0x1F
                slots.append((addr, imm12 * 4, 4, rt))

        # STRB/LDRB 8位: size=00
        elif (w & 0xFFC00000) == 0x39000000:
            rn = (w >> 5) & 0x1F
            if rn == 31:
                imm12 = (w >> 10) & 0xFFF
                rt = w & 0x1F
                slots.append((addr, imm12, 1, rt))
    return slots
```

**槽位记录**：取「首次写入」的位置作为 `insnOff`，供 `ii_local_slot` 返回。

---

### 4.5 `fieldacc` —— 字段访问

**目标**：找出 getter / setter 函数体内对 `this` 指针的字段访问。

**模式识别**：
```
函数入口：
  mov  x19, x0              ; 保存 this
  或 stp  x0, x30, [sp, ...]

函数体内：
  ldr  w0, [x0, #0x250]     ; 读 this->field
  或 ldr  x1, [x19, #0x250]
  或 str  w2, [x0, #0x250]  ; 写 this->field
```

**扫描方法**：
```python
def scan_field_access(so_path, func_offset, func_size, this_reg=0):
    """this_reg: 保存 this 指针的寄存器（通常是 x0 或 x19）"""
    fields = []
    for off in range(0, func_size, 4):
        addr = func_offset + off
        f.seek(addr)
        w = struct.unpack('<I', f.read(4))[0]

        # LDR/STR (unsigned offset) 各类宽度
        for mask, base, size, shift in [
            (0xFFC00000, 0xF9000000, 8, 3),   # 64位
            (0xFFC00000, 0xB9000000, 4, 2),   # 32位
            (0xFFC00000, 0x79000000, 2, 1),   # 16位
            (0xFFC00000, 0x39000000, 1, 0),   # 8位
        ]:
            if (w & mask) == base:
                rn = (w >> 5) & 0x1F
                if rn == this_reg:
                    imm12 = (w >> 10) & 0xFFF
                    offset = imm12 << shift
                    is_write = bool((w >> 22) & 1)   # L位：0=store, 1=load
                    fields.append((addr, offset, this_reg, size, 0 if is_write else 1))
    return fields
```

**基址寄存器**：`0` 表示 `x0`（this），`1` 表示 `x1`（第二参数）。

**验证**：与 IDA/Ghidra 反编译结果交叉比对，确认字段偏移正确。

---

### 4.6 `poolhint` —— 内存池提示

**目标**：找出 `.so` 内可用作 shell 缓冲区的零填充区。

**要求**：
1. 该区域运行时内容全为 `0`（`.bss` 尾部或代码段对齐空隙）
2. 至少 4 KB（1 页）
3. 距离被 hook 地址在 ±128 MB 内（4 字节 `b` 跳转范围）

**扫描方法**：
```python
def find_pool_hints(so_path, min_size=4096):
    """在 .bss 和代码段里找连续零区"""
    hints = []
    for seg_offset, seg_size in get_load_segments(so_path):
        start = None
        for off in range(0, seg_size, 4096):
            f.seek(seg_offset + off)
            chunk = f.read(min(4096, seg_size - off))
            if all(b == 0 for b in chunk):          # 全零页
                if start is None:
                    start = off
            else:
                if start is not None and off - start >= min_size:
                    hints.append((seg_offset + start, off - start))
                start = None
    return hints
```

**运行时验证**（重要）：`/proc/<pid>/mem` 读取实际内容确认。

```python
def verify_hint_runtime(pid, base, vaddr, size):
    fd = os.open(f'/proc/{pid}/mem', os.O_RDONLY)
    os.lseek(fd, base + vaddr, 0)
    data = os.read(fd, min(size, 4096))
    return all(b == 0 for b in data)
```

**安全约束**：`poolhint` 指向游戏自己的内存，引擎**不得往其中写任何标记**（`noGuard=1`），存活检测用 `mincore()`。

---

## 五、打包

### 5.1 准备表头文件

采集结果先写成 C 头文件，格式如：

```c
// retpoint_db.h
static const struct { uint32_t funcIdx; uint8_t isLast; uint32_t retOff; uint32_t pad; }
g_retpoint[] = {
    {  0, 0, 0x000002a4, 0 },
    {  0, 1, 0x000003b8, 0 },
    ...
};
#define N_RETPOINT (sizeof g_retpoint / sizeof g_retpoint[0])
```

### 5.2 执行打包

```bash
python3 tools/pack_db.py \
    --out data/zh_db_full.bin \
    --ver 3.10.5.301327 \
    data/*.h
```

**输出**：
```
  blcall: 449 条
  constfunc: 121 条
  const: 26447 条
  fieldacc: 716 条
  slot: 547 条
  frame: 120 条
  poolhint: 5 条
  retfunc: 124 条
  retpoint: 2754 条
  rettype: 124 条

写出 data/zh_db_full.bin: 367,193 字节
  10 张表，字符串池 2,996 字节
```

### 5.3 校验

```bash
python3 tools/verify_db.py data/zh_db_full.bin
```

**必须全部 `✅`**：
```
  rettype      打包    124  源    124  ✅
  blcall       打包    449  源    449  ✅
  const        打包  26447  源  26447  ✅
  ...
全部一致
```

---

## 六、文件格式

### 6.1 头部（32 字节）

```c
struct zh_db_hdr {
    char     magic[4];      // "ZDB1"
    uint32_t fmtVer;        // 1
    uint32_t nTables;       // 表数
    uint32_t strPoolOff;    // 字符串池偏移
    uint32_t strPoolLen;    // 字符串池长度
    uint32_t gameVerOff;    // 版本串偏移
    uint32_t gameVerLen;    // 版本串长度
    uint32_t checksum;      // 校验和
};
```

### 6.2 表目录（每张 16 字节）

```c
struct zh_db_tbl_ent {
    uint32_t type;          // 表类型（1..11）
    uint32_t count;         // 条目数
    uint32_t offset;        // 数据起始偏移
    uint32_t entrySize;     // 每条字节数
};
```

### 6.3 各表条目格式

| 类型 | 名称 | entrySize | 字段布局 |
|---|---|---|---|
| 1 | `rettype` | 16 | `u32 nameOff, u32 funcOff, u8 kind, u8 pad[3], u32 typeNameOff, ...` |
| 2 | `blcall` | 16 | `u32 hostOff, u32 callOff, u32 targetOff, u32 targetAddrOff` |
| 3 | `const` | 12 | `u16 funcIdx, u8 kind, u8 width, u32 insnOff, u32 value` |
| 4 | `frame` | 12 | `u32 funcOff, u32 frameSize, u32 slotStart, u16 slotCount, u16 pad` |
| 5 | `slot` | 8 | `u16 spOff, u8 size, u8 pad, u32 insnOff` |
| 6 | `fieldacc` | 12 | `u32 funcOff, u16 off, u8 base, u8 size, u8 wr, u8 pad[3]` |
| 7 | `funcinfo` | 4 | `u32 nameOff` |
| 8 | `constfunc` | 4 | `u32 nameOff` |
| 9 | `poolhint` | 8 | `u32 vaddr, u32 size` |
| 10 | `retfunc` | 4 | `u32 nameOff` |
| 11 | `retpoint` | 8 | `u32 funcIdx, u8 isLast, u8 pad[3], u32 retOff` |

**注意**：`entrySize` 必须与 `struct.pack` 实际写入字节数一致 —— 曾因 `fieldacc` 少写 2 字节导致整表错位。

---

## 七、版本更新流程

```
1. 拉取新版 libminecraftpe.so
        │
2. 提取符号表 (readelf)
        │
3. 跑采集脚本（见第四节）
        │
4. 生成 data/*.h
        │
5. pack_db.py 打包
        │
6. verify_db.py 校验
        │
7. 真机验证（先跑 t02 函数查找 124/124）
        │
8. 逐项跑 t03..t13
```

### 快速冒烟检查

```bash
# 部署新 bin 后，先确认基本能力
./zhinf <pid> libzh_engine.so <mmap> <dlopen>
# 再注入 t02（函数查找）
./zhinf <pid> libt02_find.so <mmap> <dlopen>
# 期望：124 / 124 全通过
```

---

## 八、常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| `zh_find` 返回 0 | 符号表版本不匹配 | 确认 `.so` 版本与数据表版本一致 |
| 函数查找 124/124 只剩十几个 | 符号提取时用了 `.dynsym` 而非 `.symtab` | 两个都试，取交集 |
| hook 后游戏崩溃 | 偏移错位 | 用 IDA 交叉验证该函数反汇编 |
| 字段读出来是垃圾 | `base` 寄存器标错 | 确认 getter 里 `this` 存在哪个寄存器 |
| `poolhint` 失效 | 游戏更新后 `.bss` 布局变了 | 重新扫描零填充区 |
| 打包后条目数对但内容错 | `entrySize` 与 `struct.pack` 不匹配 | 核对第六节表格 |

---

## 九、工具位置

```
zh_engine/tools/
├── pack_db.py          表头 → 二进制打包
└── verify_db.py        打包结果与源数据比对

zh_sym/                 采集脚本
├── scanall.py          全量函数扫描
├── slotscan.py         栈槽位扫描
├── slottype.py         槽位类型推断
├── constfp.py          常量指纹
├── xmatch2.py          跨版本函数匹配
├── verify_getters.py   getter 风险评估
└── ...                 共 34 个
```
