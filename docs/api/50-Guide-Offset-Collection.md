# 指南：偏移采集

游戏每更新一次，所有偏移都变。本文讲怎么重新采集。

---

## 一、你需要哪些数据

| 表 | 采集难度 | 自动化程度 |
|---|---|---|
| `rettype` | 中 | 半自动（符号 + 反汇编） |
| `retfunc` | 同上 | 同上 |
| `blcall` | 低 | 全自动（扫 `bl` 指令） |
| `const` | 低 | 全自动（扫立即数指令） |
| `constfunc` | 低 | 全自动 |
| `frame` | 中 | 全自动（扫 `sub sp, sp, #N`） |
| `slot` | 中 | 全自动（扫 `[sp, #N]`） |
| `fieldacc` | 低 | 全自动（扫 `ldr/str [xN, #off]`） |
| `poolhint` | 高 | 半自动（真机内存映射） |
| `retpoint` | 中 | 全自动（扫 `ret` 前位置） |

---

## 二、准备

### 2.1 拿 `.so`

```bash
adb shell su -c "ls -l /data/app/*/com.netease.x19-*/lib/arm64/libminecraftpe.so"
```

**当前版本**：

```
-rw-r--r-- 1 root root 323630848 ... libminecraftpe.so
```

**323,630,848 字节**

```bash
adb pull /data/app/.../lib/arm64/libminecraftpe.so
```

### 2.2 工具

| 工具 | 用途 |
|---|---|
| `capstone` | 反汇编（Python） |
| `pyelftools` | 解析 ELF |
| `objdump`（aarch64-linux-gnu） | 备用 |

**安装**：

```bash
pip install capstone pyelftools
```

---

## 三、第一步：确认版本

### 3.1 大小 + 哈希

```bash
ls -l libminecraftpe.so
sha256sum libminecraftpe.so
```

**记录到数据表头**（引擎启动时比对）。

### 3.2 符号表（如果有）

```python
from elftools.elf.elffile import ELFFile

with open('libminecraftpe.so', 'rb') as f:
    elf = ELFFile(f)
    symtab = elf.get_section_by_name('.symtab')
    if symtab:
        for s in symtab.iter_symbols():
            if s.name and s['st_info']['type'] == 'STT_FUNC':
                print(f"{s['st_value']:016x}  {s.name}")
```

**大多数 NetEase 版本已 strip，`.symtab` 为空。**

### 3.3 用字符串定位

```python
import re
data = open('libminecraftpe.so','rb').read()

# 找函数名
for name in [b'LocalPlayer', b'normalTick', b'ItemStack', b'isNull']:
    for m in re.finditer(re.escape(name), data):
        print(f"0x{m.start():08x}  {name}")
```

**字符串本身不给地址** —— 但 `adrp + add` 引用它，可以反推。

---

## 四、找函数地址

### 4.1 方法 1：`adrp` + `add` 模式

```python
from capstone import *

def find_string_refs(so_path, needle):
    """找 adrp+add 引用某字符串的指令，返回引用点的地址"""
    data = open(so_path,'rb').read()

    # 1. 找字符串在本文件中的偏移
    idx = data.find(needle)
    if idx < 0:
        return []

    # 2. 字符串的虚拟地址（需要 .text 的 vaddr == file offset 前提，
    #    或者从 ELF 段表换算）
    str_vaddr = file_off_to_vaddr(idx)

    # 3. 扫所有 adrp + add 组合
    #    adrp xN, page  → xN = page
    #    add  xN, xN, #lo12 → xN = 完整地址
    page = str_vaddr & ~0xFFF
    lo12 = str_vaddr & 0xFFF

    hits = []
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    for i in range(0, len(data) - 8, 4):
        w1 = u32(data, i)
        if (w1 & 0x9F00001F) != 0x90000000:   # adrp
            continue
        w2 = u32(data, i+4)
        if (w2 & 0xFFC003FF) != 0x91000000:   # add (immediate)
            continue
        if not adrp_add_matches(w1, w2, str_vaddr):
            continue
        hits.append(addr_to_vaddr(i))

    return hits
```

### 4.2 从引用点反推函数入口

**引用点在函数体内。要找到函数入口，往回扫到函数序言：**

```python
def find_func_start(so_path, ref_vaddr, max_back=0x1000):
    """从引用点往回找函数序言（stp x29,x30,[sp,#-N]! 或 sub sp,sp,#N）"""
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True

    for back in range(0, max_back, 4):
        v = ref_vaddr - back
        insns = list(md.disasm(read_at(so_path, vaddr_to_off(v)), 4, v))
        if not insns: continue
        i = insns[0]

        # 序言特征
        if i.id in (ARM64_INS_STP, ARM64_INS_SUB) and i.op_str.startswith('sp'):
            return v
        if i.id == ARM64_INS_STP and 'x29, x30' in i.op_str:
            return v

    return 0
```

### 4.3 验证：函数名与行为匹配

**找到地址后，一定要反汇编看是不是符合名字：**

```python
def disasm_func(so_path, vaddr, nmax=40):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    code = read_at(so_path, vaddr_to_off(vaddr), nmax*4)
    for i in md.disasm(code, vaddr):
        print(f"  {i.address:08x}: {i.bytes.hex():8s} {i.mnemonic:8s} {i.op_str}")
```

**例**：`Actor_getLevel` 应该是

```asm
  5f2a1b0: fd7bbfa9  stp  x29, x30, [sp, #-0x10]!
  5f2a1b4: fd030091  mov  x29, sp
  5f2a1b8: 005440f9  ldr  x0, [x0, #0x250]     ← 读字段
  5f2a1bc: fd7bc1a8  ldp  x29, x30, [sp], #0x10
  5f2a1c0: c0035fd6  ret
```

**如果反汇编出来完全不是这个形状，地址就错了。**

---

## 五、`blcall`：全自动

**扫所有 `bl` 指令，解出目标，如果双方都在已知函数表里就记录。**

```python
FUNC_MAP = {}   # vaddr -> name

def scan_blcalls(so_path, text_vaddr, text_size):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    code = read_at(so_path, vaddr_to_off(text_vaddr), text_size)

    out = []
    for i in md.disasm(code, text_vaddr):
        if i.id != ARM64_INS_BL:
            continue
        target = int(i.op_str, 16)

        from_name = FUNC_MAP.get(i.address)
        to_name   = FUNC_MAP.get(target)
        if not from_name:
            # 往上找最近的函数入口
            from_name = nearest_func(i.address)
        if not to_name:
            continue

        out.append((from_name, i.address - FUNC_MAP_INV[from_name],
                    to_name, target))
    return out
```

**实测 449 条。**

---

## 六、`const`：全自动

**扫立即数指令，在已知函数范围内记录。**

```python
def scan_consts(so_path, funcs):
    """funcs: [(name, start_vaddr, end_vaddr)]"""
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True

    out = []
    for name, start, end in funcs:
        code = read_at(so_path, vaddr_to_off(start), end - start)

        for i in md.disasm(code, start):
            if i.id not in (ARM64_INS_MOVZ, ARM64_INS_MOVK, ARM64_INS_MOVN,
                            ARM64_INS_CMP, ARM64_INS_SUBS,
                            ARM64_INS_ADD, ARM64_INS_SUB):
                continue

            imm = extract_imm(i)
            if imm is None or imm == 0:
                continue

            out.append((
                FUNC_IDX[name],
                imm_kind(i.id),      # 0=cmp 1=mov 2=addsub
                imm_width(i),        # 1/2/4/8
                i.address - start,
                imm & 0xFFFFFFFF
            ))
    return out
```

**实测 26,447 条。**

**注意 `movz + movk` 组合**：大常量分两条指令，要合并：

```python
def extract_imm(insn):
    for op in insn.operands:
        if op.type == ARM64_OP_IMM:
            return op.imm
    return None
```

---

## 七、`frame`：全自动

**找函数序言的 `sub sp, sp, #N` 或 `stp ..., [sp, #-N]!`。**

```python
def scan_frames(so_path, funcs):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True

    out = []
    for name, start, end in funcs:
        code = read_at(so_path, vaddr_to_off(start), min(end-start, 64))

        frameSize = 0
        for i in md.disasm(code, start):
            if i.id == ARM64_INS_SUB and 'sp, sp' in i.op_str:
                frameSize = extract_imm(i) or 0
                break
            if i.id == ARM64_INS_STP and i.op_str.endswith('!]'):
                # stp x29, x30, [sp, #-0x20]!
                frameSize = abs(extract_pre_index(i) or 0)
                break

        if frameSize > 0:
            out.append((name, frameSize))
    return out
```

**实测 120 条。**

---

## 八、`slot`：全自动

**扫 `[sp, #N]` 形式的访问，排除 `stp/ldp` 的成对访问（那些是保存寄存器）。**

```python
def scan_slots(so_path, funcs, frame_map):
    out = []
    for name, start, end in funcs:
        if name not in frame_map: continue
        fs = frame_map[name]
        if fs == 0: continue

        code = read_at(so_path, vaddr_to_off(start), end - start)
        seen = {}

        for i in md.disasm(code, start):
            off = extract_sp_offset(i)
            if off is None: continue
            if off < 0 or off >= fs: continue        # 越界
            if is_prologue_save(i): continue         # 序言保存

            size = extract_access_size(i)
            if off in seen:
                if i.address < seen[off]: seen[off] = i.address
            else:
                seen[off] = i.address

        for off, insn_addr in sorted(seen.items()):
            out.append((name, off, size, insn_addr - start))
    return out
```

**实测 547 条。**

---

## 九、`fieldacc`：全自动

**扫 `ldr/str [xN, #off]`，如果函数只做这件事（简单 getter），就是字段访问。**

```python
def scan_fieldacc(so_path, funcs):
    out = []
    for name, start, end in funcs:
        code = read_at(so_path, vaddr_to_off(start), end - start)

        # 简单 getter 模式：
        #   ldr x0, [x0, #off]
        #   ret
        insns = list(md.disasm(code, start))
        body = [i for i in insns
                if i.id not in (ARM64_INS_STP, ARM64_INS_LDP,
                                ARM64_INS_MOV, ARM64_INS_RET)]

        if len(body) == 1 and body[0].id in (ARM64_INS_LDR, ARM64_INS_LDRB,
                                             ARM64_INS_LDRH, ARM64_INS_LDRSW):
            i = body[0]
            base, off = extract_mem(i)
            out.append((name, off, base, access_size(i), 0))
        elif len(body) == 1 and body[0].id in (ARM64_INS_STR, ARM64_INS_STRB):
            i = body[0]
            base, off = extract_mem(i)
            out.append((name, off, base, access_size(i), 1))
    return out
```

**实测 716 条。**

---

## 十、`retpoint`：全自动

**扫所有 `ret` 指令，如果它属于某个已知函数就记录。**

```python
def scan_retpoints(so_path, funcs):
    out = []
    for name, start, end in funcs:
        code = read_at(so_path, vaddr_to_off(start), end - start)

        for i in md.disasm(code, start):
            if i.id != ARM64_INS_RET:
                continue
            out.append((FUNC_IDX[name], i.address - start))
    return out
```

**实测 2,754 条**（平均每函数 22 个）。

---

## 十一、`poolhint`：最难，需要真机

**要找"±128 MB 内、有 100KB+ 空白的可写内存"。**

### 11.1 真机扫描

```bash
adb shell "cat /proc/<pid>/maps" | grep libminecraftpe
```

**或者用 Nexus 自己的扫描**（更准）：

```c
// 探针里调
int scan_pool_for_target(zh_find("LocalPlayer_normalTick"));
```

### 11.2 记录结果

**检查每个池是否覆盖了 `±128 MB` 的全部 target**：

```python
def check_pool_coverage(targets, pools):
    uncovered = []
    for t in targets:
        ok = any(abs(p - t) < (1<<27) for p, sz in pools)
        if not ok:
            uncovered.append(t)
    return uncovered
```

**如果 `uncovered` 非空 → 池不够**，要补。

### 11.3 已验证的 5 个池

```c
static const zh_poolhint_t k_poolhint[] = {
    { 0x1334b08c, 308*1024 },
    { 0x1b3f2000, 244*1024 },
    { 0x2c8e1000,  20*1024 },
    { 0x3a4d5000,   8*1024 },
    { 0x4f2c8000,   7*1024 },
};
```

**这些是相对模块基址的偏移。**

---

## 十二、完整流程脚本

```bash
# 1. 拉 .so
adb pull .../libminecraftpe.so

# 2. 找函数地址
python3 extract_sem.py --so libminecraftpe.so --names names.txt -o funcs.json

# 3. 扫所有表
python3 scan_blcalls.py  --so libminecraftpe.so --funcs funcs.json -o blcall.h
python3 scan_consts.py   --so libminecraftpe.so --funcs funcs.json -o const.h
python3 scan_frames.py   --so libminecraftpe.so --funcs funcs.json -o frame.h
python3 scan_slots.py    --so libminecraftpe.so --funcs funcs.json --frames frame.h -o slot.h
python3 scan_fields.py   --so libminecraftpe.so --funcs funcs.json -o field.h
python3 scan_retpoints.py --so libminecraftpe.so --funcs funcs.json -o retpoint.h

# 4. 打包
python3 tools/pack_db.py data -o data/zh_db_full.bin

# 5. 校验
python3 tools/verify_db.py data/zh_db_full.bin
```

---

## 十三、验证采集结果

### 13.1 静态验证（必须）

```bash
python3 _pyverify_all.py
```

**检查项**：
- 所有偏移在 `.so` 范围内
- 所有函数名可打印
- 所有 `funcIdx` 有效
- `fieldacc` 的 `off` 在合理范围（< 0x1000）
- `frame` 的 `size` 是 16 的倍数（通常）

### 13.2 交叉验证

**用不同方法找同一个函数，看结果是否一致：**

```python
# 方法 A：字符串引用
addr_a = find_by_string_ref('LocalPlayer_normalTick')

# 方法 B：调用关系（谁调了它）
addr_b = find_by_caller('Level_tick')

assert addr_a == addr_b, "两种方法结果不一致！"
```

### 13.3 真机验证

**装最小的探针，只验证 `zh_find` 能拿到正确地址：**

```c
// t02 就是这个
for each func in retfunc:
    addr = zh_find(func);
    if (!addr) { FAIL("找不到"); }
    if (addr < base || addr > base + soSize) { FAIL("越界"); }
    w = *(uint32_t*)addr;
    if (w == 0 || w == 0xFFFFFFFF) { FAIL("指令无效"); }
```

**实测 t02：124/124 通过。**

---

## 十四、常见坑

| 坑 | 表现 | 修法 |
|---|---|---|
| `fieldacc` 记录宽度不对 | 整表错位 | 用 `<IHBBBxxx` |
| 字符串偏移算错 | `zh_find` 返回错误地址 | 校验 ELF 段映射 |
| `movz+movk` 没合并 | 常量值错误 | 合并两条指令 |
| 函数边界算错 | `const` 记录到别的函数 | 用符号或序言边界 |
| 未验证就打包 | 真机全崩 | 必须跑 `_pyverify_all.py` |

---

## 十五、迭代策略

**不要一次采全部。**

```
第 1 轮：只采 10 个核心函数 → 真机验证 → 通过
第 2 轮：加到 40 个 → 验证
第 3 轮：加到 124 个 → 验证
第 4 轮：加所有表 → 验证
```

**每轮都要真机跑一遍。**

---

## 下一步

- [版本更新](51-Guide-Version-Update)
- [常见陷阱](52-Guide-Pitfalls)
- [数据格式](42-Reference-Data-Format)
