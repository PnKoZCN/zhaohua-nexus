# 核心概念：指令改写原理

本页讲清楚：**Nexus 如何在运行时改变游戏行为**。

---

## 一、最基本的思路

**把目标函数的入口指令改成「跳转」指令，跳到我们自己的代码。**

```
改之前：
  0x7c4cfba6c8:  fc190fe8    stp x29, x30, [sp, #-16]!    ← 原指令
  0x7c4cfba6cc:  a9017bfd    stp x29, x30, [sp, #16]
  ...

改之后：
  0x7c4cfba6c8:  14000000+   b shell                       ← 换成跳转
  0x7c4cfba6cc:  a9017bfd    stp x29, x30, [sp, #16]       ← 不动
  ...
```

**问题**：原指令 `stp` 被覆盖了，函数的栈帧就不对了。

**解决**：跳转前先把原指令搬到别处执行（**trampoline**），执行完再跳回来。

---

## 二、完整链路

```
调用者 bl 目标函数
    │
    ▼
目标函数入口（已被改写）
    b shell   ← 4 字节原子写
    │
    ▼
shell（我们 mmap 出来的 24 字节）
    stp  x16, x17, [sp, #-16]!     ; 保存 x16/x17（它们是临时寄存器）
    movz x17, #sid                 ; 传站点号
    ldr  x16, #8                   ; 装载 handler 地址
    br   x16
    .quad ii_common                ; handler
    │
    ▼
ii_common（ARM64 汇编，栈帧 0x320）
    sub  sp, sp, #0x320
    stp  x0,  x1,  [sp, #0x000]    ; 保存所有寄存器
    ...                            ; x0-x30, q0-q31, fpsr, fpcr, pstate
    mov  x0, sp                    ; ctx = 栈帧地址
    mov  x1, x17                   ; slotId = sid
    bl   ii_dispatch               ; ← C 层：遍历回调链
    str  x0, [sp, #0x100]          ; 保存 trampoline 地址
    ...                            ; 恢复所有寄存器
    ldr  x16, [sp, #0x100]         ; x16 = trampoline
    cbz  x16, 9f                   ; NULL 兜底
    br   x16
9:  ret
    │
    ▼
trampoline（我们 mmap 出来的，最多 44 字节）
    ldr  x16, [sp, #0x080]         ; 恢复原 x16
    ldr  x17, [sp, #0x088]         ; 恢复原 x17
    add  sp, sp, #0x330            ; 恢复 sp
    fc190fe8                       ; ← 执行原指令
    b    addr+4                    ; 跳回函数第 2 条指令
```

---

## 三、关键设计

### 3.1 为什么用 `b` 而不是 `bl`

**`b`（4 字节，±128MB 跳转）**

```asm
b <offset26>     ; 0x14000000 | (imm26 & 0x03FFFFFF)
```

**不用 `bl`** —— 因为 `bl` 会写 `x30`（返回地址），破坏调用者的栈帧。

**代价**：只能跳 ±128 MB。

### 3.2 ±128MB 的限制

**问题**：游戏 `.so` 附近 ±128 MB 内可能没有空闲页。

实测：`libminecraftpe.so` 周围连 64 KB 空洞都没有。

**解决**：`poolhint` 表指向游戏 `.so` 内部的**零填充区**（如 `.bss` 尾部）：

| vaddr | size | 位置 |
|---|---|---|
| `+0x1334b08c` | 308 KB | `.bss` |
| `+0x136e08fc` | 244 KB | `.bss` |
| `+0x13763d00` | 20 KB | `.bss` |
| `+0x387a304` | 8 KB | 代码段空隙 |
| `+0x3856e94` | 7 KB | 代码段空隙 |

**这些区域运行时内容全为 0**，可以当 shell 缓冲区。

**安全约束**：写它等于改游戏内存 → 引擎**绝不往这里写标记**（`noGuard = 1`），存活检测用 `mincore()`（只读）。

### 3.3 16 字节模式的兜底

**如果连 `poolhint` 都用不了**（比如都被占用了），退回**16 字节改写**：

```asm
addr+0x00:  ldr  x16, #8         ; 0x58000050
addr+0x04:  br   x16             ; 0xD61F0200
addr+0x08:  .quad shell          ; shell 可以放在任意远处
（覆盖 16 字节 = 4 条指令）
```

**代价**：
- 要搬走 3 条原指令（不是 1 条）
- 非原子写，**必须暂停所有线程**
- 需要 `can_cover_16()` 检查（4 条指令都不能碰 `x16/x17`、不能是分支）

**`can_cover_16` 的检查**：

```c
for (int i = 0; i < 4; ++i) {
    uint32_t w = *(volatile uint32_t*)(addr + i*4);
    if (uses_ip0_ip1(w)) return 0;              // 用了 x16/x17
    if (w == 0 || w == 0xFFFFFFFFu) return 0;   // 无效指令
    if ((w & 0xFFFFFC1F) == 0xD65F0000) return 0;  // ret
    if ((w & 0xFC000000) == 0x14000000) return 0;  // b
    if ((w & 0xFF000010) == 0x54000000) return 0;  // b.cond
    if ((w & 0x7E000000) == 0x34000000) return 0;  // cbz
    if ((w & 0x7E000000) == 0x36000000) return 0;  // tbz
    if ((w & 0x3B000000) == 0x18000000) return 0;  // ldr-literal
    if ((w & 0x1F000000) == 0x10000000) return 0;  // adrp/adr
}
```

**为什么不能碰 `x16/x17`**：trampoline 在远处，跳回时用 `ldr x16, #8; br x16`，这两条会覆盖 `x16/x17`。

---

## 四、栈平衡（最容易出错的地方）

### 4.1 三处 sp 调整

```
shell:       stp x16,x17,[sp,#-16]!     sp -= 16
ii_common:   sub sp, sp, #0x320          sp -= 0x320  → 合计 -0x330
trampoline:  add sp, sp, #0x330          sp += 0x330  → 回到原值
```

**净变化 = 0。**

### 4.2 为什么 shell 要压栈

**保存原 `x16/x17`**：

`ii_common` 用 `x16` 装 handler 地址、`x17` 装站点号，会覆盖原值。但被 hook 的函数可能依赖原 `x16/x17`（虽然 AAPCS64 说它们是临时寄存器，实际代码可能持有值）。

**压栈保存后，`ii_common` 从栈上取回原值**：

```asm
; ii_common 里
ldr  x10, [sp, #0x320]     ; = [S-16] = 原 x16（S 是进入 ii_common 前的 sp）
str  x10, [sp, #0x080]     ; 覆盖保存区

ldr  x10, [sp, #0x328]     ; = [S-8] = 原 x17
str  x10, [sp, #0x088]
```

**trampoline 再从这里恢复**：

```asm
ldr  x16, [sp, #0x080]
ldr  x17, [sp, #0x088]
```

### 4.3 验证栈平衡

真机上可以这样验证：

```c
static void cb(ii_ctx* c, void* user) {
    static unsigned long last_sp = 0;
    if (last_sp && last_sp != c->sp) {
        LOG_E(LOG_HOOK, "栈漂移！%lx → %lx", last_sp, c->sp);
    }
    last_sp = c->sp;
}
```

**期望**：`c->sp` 每次相同（同一个调用点）。

---

## 五、`ii_ctx` 结构

**这是回调能看到的"现场"**：

```c
typedef struct {
    uint64_t   x[31];      // 偏移 0x000，x0-x30
    uint64_t   sp;         // 偏移 0x0F8
    uint64_t   pc;         // 偏移 0x100（被插桩指令的地址）
    uint64_t   pstate;     // 偏移 0x108（nzcv）
    __uint128_t q[32];     // 偏移 0x110，q0-q31
    uint64_t   fpsr;       // 偏移 0x310
    uint64_t   fpcr;       // 偏移 0x318
} ii_ctx;                  // 共 0x320 字节
```

**常见访问**：

```c
c->x[0]    // this（C++ 成员函数）或第一个参数
c->x[1]    // 第一个参数（如果是成员函数则是第二个）
c->x[2]    // ...
c->sp      // 栈指针
c->pc      // 当前指令地址
```

**注意**：`ii_common` 把 `ctx` 放在**自己栈帧的起点**（`sp`），所以：

```
ctx = sp = S - 0x330
[ctx + 0x080] = 原 x16
[ctx + 0x088] = 原 x17
```

---

## 六、LIGHT 模式

**高频函数用 FULL 模式开销大**（保存 32 个 q 寄存器 = 512 字节）。

**LIGHT 模式只保存必要部分**：

| 模式 | 保存内容 | 栈帧 | 适用 |
|---|---|---|---|
| FULL | x0-x30, q0-q31, fpsr, fpcr, pstate | 0x320 | 通用 |
| LIGHT | x0-x7, q0-q1 | 96 B | 高频、只读参数 |

```c
// FULL
zh_hook("func", cb, user);        // ii_ctx*

// LIGHT（更轻）
zh_hook_light("func", lcb, user); // ii_ctx_light*
```

**`ii_ctx_light`**：

```c
typedef struct {
    uint64_t x[8];      // x0-x7
    // 后面是 q0-q1（32 字节）
} ii_ctx_light;
```

**LIGHT 的限制**：
- 不能 cancel（没有 pstate）
- 不能改返回值（返回地址不在 ctx 里）
- 只看得到前 8 个参数

**实测**：`LocalPlayer_normalTick` 用 LIGHT 回调 20 TPS 稳定运行。

---

## 七、指令编码速查

### 7.1 用到的编码

| 指令 | 编码 | 说明 |
|---|---|---|
| `b <off>` | `0x14000000 \| (imm26 & 0x03FFFFFF)` | ±128 MB |
| `bl <off>` | `0x94000000 \| (imm26 & 0x03FFFFFF)` | 调用 |
| `ldr xt, #imm` | `0x58000000 \| ((imm>>2)<<5) \| rt` | 加载字面量 |
| `br xn` | `0xD61F0000 \| (rn << 5)` | 跳转 |
| `ret` | `0xD65F03C0` | 返回 |
| `movz xd, #imm16` | `0xD2800000 \| (imm16<<5) \| rd` | 装载 |
| `stp xt,xt2,[sp,#-N]!` | `0x29800000 \| ...` | 前索引压栈 |
| `add sp,sp,#imm` | `0x910003FF \| (imm12<<10)` | 调整栈 |

### 7.2 `enc_b` 实现

```c
static uint32_t enc_b(uint64_t from, uint64_t to) {
    int64_t off = (int64_t)to - (int64_t)from;
    if (off < -(1LL<<27) || off >= (1LL<<27)) return 0;   // 超出范围
    if (off & 3) return 0;                                 // 必须 4 字节对齐
    uint32_t imm26 = (uint32_t)((off >> 2) & 0x03FFFFFF);
    return 0x14000000u | imm26;
}
```

### 7.3 `enc_movz_x17`

```c
static uint32_t enc_movz_x17(uint32_t imm16) {
    return 0xD2800000u | ((imm16 & 0xFFFF) << 5) | 17u;
}
```

---

## 八、写入指令的安全要求

### 8.1 4 字节原子写

**ARM64 对齐的 32 位写是原子的**：

```c
static int patch32(unsigned long addr, uint32_t insn) {
    if (!make_writable(addr, 4)) return -1;
    *(volatile uint32_t*)addr = insn;      // ← 原子
    __builtin___clear_cache((char*)addr, (char*)addr + 4);
    make_readable(addr, 4);
    return 0;
}
```

**为什么不需要暂停线程**：

跑到这里的线程要么已取指（继续按旧指令走），要么还没到（下次取到新指令）。**不存在半写状态**。

**实测**：4 字节路径安装/卸载零卡顿。

### 8.2 16 字节必须暂停

**16 字节非原子写** → 必须确保所有线程不在执行这 16 字节：

```c
int stopped = suspend_others();
if (stopped < g_stopExpect) {
    resume_others();
    return -17;   // 停不全就放弃
}
memcpy((void*)addr, patch, 16);
__builtin___clear_cache((char*)addr, (char*)addr + 16);
resume_others();
```

**暂停机制**：
- 向所有线程发 `SIGRTMIN+3`
- handler 里用 futex 等待（不烧 CPU）
- 恢复时 `FUTEX_WAKE` 唤醒

**风险**：停不全（D 状态线程）→ 放弃安装。

**历史教训**：早期版本暂停状态泄漏，导致全进程线程永久停在 `do_signal_stop` → 整机卡死 + ANR。

### 8.3 W^X（可写与可执行互斥）

Android 有 W^X 保护：

```c
make_writable(addr, len);    // mprotect RW
// 写指令
make_readable(addr, len);    // mprotect RX
```

**不能设成 RWX** —— 会被拒绝（EACCES）。

---

## 九、完整示例

**假设要 hook `0x7c4cfba6c8`：**

```c
// 1. 读原指令
uint32_t orig = *(volatile uint32_t*)0x7c4cfba6c8;   // 0xfc190fe8

// 2. 分配 shell（假设从 poolhint 拿到 0x7c42eb2010）
void* shell = (void*)0x7c42eb2010;
void* tramp = (void*)((char*)shell + 32);    // slot+32

// 3. 写 shell
uint32_t* sh = (uint32_t*)shell;
sh[0] = 0xA9BF47F0;         // stp x16,x17,[sp,#-16]!
sh[1] = enc_movz_x17(sid);  // movz x17, #sid
sh[2] = 0x58000050;         // ldr x16, #8
sh[3] = 0xD61F0200;         // br x16
*(uint64_t*)(sh+4) = (uint64_t)ii_common;

// 4. 写 trampoline（普通指令模式）
uint32_t* t = (uint32_t*)tramp;
t[0] = 0xF9400000 | ((0x080>>3)<<10) | (31<<5) | 16;  // ldr x16,[sp,#0x80]
t[1] = 0xF9400000 | ((0x088>>3)<<10) | (31<<5) | 17;  // ldr x17,[sp,#0x88]
t[2] = 0x910003FF | ((0x330&0xFFF)<<10);              // add sp,sp,#0x330
t[3] = orig;                                          // 原指令
t[4] = enc_b((uint64_t)(tramp+16), addr+4);           // b addr+4

// 5. 改入口
make_writable(addr, 4);
*(uint32_t*)addr = enc_b(addr, (uint64_t)shell);
__builtin___clear_cache((char*)addr, (char*)addr+4);
make_readable(addr, 4);
```

**引擎把这 5 步封装成 `site_enable(s, sid)`。**

---

## 十、边界情况

| 情况 | 处理 |
|---|---|
| 入口是 `ret` | trampoline 只放 `ret`，不跳回 |
| 入口是 `b` | trampoline 里用 `ldr x16; br x16` 跳向原目标 |
| 入口是 `b.cond`/`cbz` | trampoline 条件分支结构，最长 44 字节 |
| 入口后 32 字节内有分支 | 4 字节模式 OK；16 字节模式需 `can_cover_16` 通过 |
| 目标超出 ±128 MB | 退 16 字节模式 |
| `poolhint` 全满 | 退 16 字节模式（`mmap(NULL)` 拿远处页） |
| 指令无法插桩（分支/PC相对） | 返回 -5 |

---

## 下一步

- [内存池与外壳](12-Core-Memory-Pool)
- [性能与开销](41-Reference-Performance)
- [常见陷阱](52-Guide-Pitfalls)
