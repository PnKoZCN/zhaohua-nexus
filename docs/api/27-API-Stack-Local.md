# API：栈局部变量

读写函数内的局部变量 —— 对标 Mixin `@Local` / `@ModifyVariable`。

---

## 一、核心 API

### 1.1 `ii_frame_size()`

```c
int ii_frame_size(const char* funcName);
```

**返回**：函数的栈帧大小（字节），0 = 未知。

```c
int fs = ii_frame_size("ItemStack_isNull");
LOG_I(LOG_LOCAL, "栈帧 %d 字节 (0x%x)", fs, fs);
```

**实测**：`ItemStack_isNull` → `0x440`（1088 字节）

---

### 1.2 `ii_local_count()`

```c
int ii_local_count(const char* funcName);
```

**返回**：该函数记录的栈槽位数。

**实测**：`ItemStack_isNull` → **18 个槽位**

---

### 1.3 `ii_local_slot()`

```c
int ii_local_slot(const char* funcName, int idx,
                  uint16_t* spOff, uint8_t* size, uint32_t* insnOff);
```

**作用**：查第 `idx` 个槽位的信息。

**参数**：
- `spOff` —— 相对 `sp` 的偏移
- `size` —— 宽度（1/2/4/8）
- `insnOff` —— 首次访问该槽位的指令偏移

**示例**：

```c
int n = ii_local_count("ItemStack_isNull");
for (int i = 0; i < n; i++) {
    uint16_t spOff; uint8_t size; uint32_t insnOff;
    if (ii_local_slot("ItemStack_isNull", i, &spOff, &size, &insnOff) != 0) continue;
    LOG_I(LOG_LOCAL, "槽[%2d] sp+0x%-4x %d 字节 (首写 @+0x%x)",
          i, spOff, size, insnOff);
}
```

**实测输出**：

```
槽[ 0] sp+0x8    1 字节 (首写 @+0x1b8)
槽[ 1] sp+0x20   1 字节 (首写 @+0x1bc)
槽[ 2] sp+0x428  8 字节 (首写 @+0x2a0)
槽[ 3] sp+0x420  8 字节 (首写 @+0x2a4)
...
```

---

### 1.4 `zh_local_get()`

```c
int64_t zh_local_get(ii_ctx* c, uint16_t spOff, uint8_t size);
```

**作用**：在 hook 里读栈局部变量。

**示例**：

```c
static void on_entry(ii_ctx* c, void* user) {
    (void)user;

    // 读 sp+0x8 的 1 字节
    int64_t v = zh_local_get(c, 0x8, 1);
    LOG_I(LOG_LOCAL, "sp+0x8 = %ld", v);
}
```

---

### 1.5 `zh_local_set()`

```c
void zh_local_set(ii_ctx* c, uint16_t spOff, uint8_t size, int64_t v);
```

**作用**：写栈局部变量。

```c
zh_local_set(c, 0x8, 4, 100);
```

---

## 二、实测案例（t07）

### 2.1 完整代码

```c
static volatile unsigned long g_hits = 0;
static volatile unsigned long g_slotVal[4] = {0};
static uint16_t g_slotOff[4];
static uint8_t  g_slotSize[4];

static void on_entry(ii_ctx* c, void* user) {
    (void)user;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);

    // 只采样一次
    static int once = 0;
    if (__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED) != 0) return;

    for (int i = 0; i < 4; ++i) {
        if (!g_slotSize[i]) continue;
        int64_t v = zh_local_get(c, g_slotOff[i], g_slotSize[i]);
        g_slotVal[i] = (unsigned long)v;
    }
}

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;

    const char* fn = "ItemStack_isNull";

    // 1. 栈帧信息
    LOG_I(LOG_LOCAL, "frame_size=0x%x", ii_frame_size(fn));
    LOG_I(LOG_LOCAL, "local_count=%d", ii_local_count(fn));

    // 2. 取前 4 个槽位
    int pick = 0;
    int n = ii_local_count(fn);
    for (int i = 0; i < n && pick < 4; i++) {
        uint16_t so; uint8_t ss; uint32_t io;
        if (ii_local_slot(fn, i, &so, &ss, &io) != 0) continue;
        g_slotOff[pick] = so;
        g_slotSize[pick] = ss;
        pick++;
    }

    // 3. 装 hook
    zh_hook(fn, on_entry, NULL);

    // 4. 观察
    sleep(3);
    LOG_I(LOG_LOCAL, "hits=%lu", __atomic_load_n(&g_hits, __ATOMIC_RELAXED));

    for (int i = 0; i < pick; i++) {
        LOG_I(LOG_LOCAL, "槽 sp+0x%-4x = 0x%lx",
              g_slotOff[i], g_slotVal[i]);
    }

    zh_unhook(fn);
    while (1) sleep(5);
    return NULL;
}
```

### 2.2 结果

```
OK   ii_frame_size          0x440 (1088 字节)
OK   ii_local_count         18 个
OK   ii_local_slot 枚举     取了 4 个
OK   zh_hook                rc=0
OK   回调被触发             74844 次 / 3 秒 ≈ 25000 次/秒
OK   zh_local_get 读取      4/4 个槽位

槽 sp+0x8    = 0xffffffffffffffe8    (-24)
槽 sp+0x20   = 0x40                  (64)
槽 sp+0x428  = 0xb400000000000000
槽 sp+0x420  = 0x42b40004
```

**这就是真实的栈局部变量值。**

---

## 三、槽位偏移的含义

### 3.1 `spOff` 是相对当前 `sp` 的偏移

**注意**：hook 时 `ii_ctx.sp` 是**函数入口的 sp**，不是当前 sp。

```c
// 函数入口时：
//   sp = S
//   局部变量在 [S-8], [S-0x20], ...

// 但数据表记录的是相对 sp 的偏移
// 如果记录 sp+0x8，实际是 [S+8]（调用者的栈帧）
```

**更常见的是负偏移**（栈向低地址增长）：

```
sp + 0x8    → 可能是 caller 的栈
sp - 0x8    → 通常是本函数的局部变量
```

**但数据表里都是正数**（如 `0x8`、`0x20`、`0x420`），因为编译器在函数入口 `sub sp, sp, #frameSize` 之后，局部变量都在 `[sp + 0..frameSize]` 范围。

**所以 `spOff` 是相对"函数体内 sp"的偏移。**

### 3.2 `ii_ctx.sp` 的语义

`ii_common` 里：

```asm
add  x9, sp, #0x330      ; ctx.sp = 进入前的 sp
str  x9, [sp, #0x0f8]
```

**`ctx.sp` = 函数入口的 sp**（`sub sp, sp, #frameSize` 之前）。

**要读局部变量，需要**：

```
实际地址 = ctx.sp - frameSize + spOff
        或
        = ctx.sp + spOff（如果数据表已考虑 frameSize）
```

**实测表明** `zh_local_get(c, spOff, size)` **直接用 `c->sp + spOff`**：

```c
int64_t ii_local_read(ii_ctx* c, uint16_t spOff, uint8_t size) {
    if (!c) return 0;
    uint64_t base = c->sp + spOff;
    switch (size) {
        case 1: return *(int8_t*)base;
        case 2: return *(int16_t*)base;
        case 4: return *(int32_t*)base;
        case 8: return *(int64_t*)base;
    }
    return 0;
}
```

**所以数据表里的 `spOff` 已经是"相对函数入口 sp 的绝对偏移"。**

---

## 四、`zh_dump_locals()`

```c
void zh_dump_locals(ii_ctx* c);
```

**作用**：调试用 —— 把 ctx 里的栈区域 dump 出来。

**用途**：手工找某个变量的位置。

```c
static void on_entry(ii_ctx* c, void* user) {
    (void)user;
    static int once = 0;
    if (__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED) == 0) {
        zh_dump_locals(c);   // 打日志
    }
}
```

---

## 五、修改局部变量

### 5.1 直接写

```c
static void on_entry(ii_ctx* c, void* user) {
    (void)user;
    // 把 sp+0x8 的 4 字节改成 100
    zh_local_set(c, 0x8, 4, 100);
}
```

### 5.2 读改写

```c
int64_t v = zh_local_get(c, 0x8, 4);
zh_local_set(c, 0x8, 4, v * 2);
```

### 5.3 时机很重要

**在入口 hook 里写，会被函数体的初始化覆盖。**

**正确做法**：在**局部变量初始化之后、使用之前**插桩。

```c
// 用 ii_const_addr 或手工找指令地址
// 然后在那个地址处 ii_install
```

**或者**：用 `ii_local_slot` 返回的 `insnOff`（首写指令），在它**之后**插桩：

```c
uint16_t spOff; uint8_t size; uint32_t insnOff;
ii_local_slot(fn, idx, &spOff, &size, &insnOff);

unsigned long func_addr = zh_find(fn);
unsigned long after_init = func_addr + insnOff + 4;   // 首写之后

ii_install(after_init, on_after_init, NULL);
```

---

## 六、CFI 支持

**更精确的栈帧信息需要 CFI（Call Frame Information）：**

```c
int ii_frame_cfi_load(const char* path);
int ii_frame_cfi_loaded(void);
```

**默认用静态扫描的数据**（`frame_db.h`，120 个函数）。

**加载 CFI 可提高精度**：

```c
int n = ii_frame_cfi_load("/path/to/frame.cfi.bin");
LOG_I(LOG_LOCAL, "CFI 加载 %d 个函数", n);
```

**CFI 的代价**：文件较大（实测 1.7 MB）。

---

## 七、限制

| 限制 | 说明 |
|---|---|
| 只有 120 个函数有帧数据 | 其余返回 0 |
| 547 个槽位记录 | 不是所有局部变量都有 |
| 编译器优化可能改变布局 | Release 构建下变量可能被寄存器化 |
| 写入时机难控制 | 需要精确的插桩点 |

---

## 八、与 Mixin 对比

| Mixin | Nexus |
|---|---|
| `@ModifyVariable(ordinal = 0)` | `zh_local_get(c, spOff, size)` |
| 按类型+序号匹配 | 按栈偏移匹配 |
| 编译器计算 | 数据表记录 |
| 类型安全 | 手工指定 size |

**Mixin**：
```java
@ModifyVariable(method = "attack", at = @At("HEAD"), ordinal = 0)
private int modifyDamage(int damage) {
    return damage * 2;
}
```

**Nexus**：
```c
static void on_attack(ii_ctx* c, void* user) {
    (void)user;
    int64_t dmg = zh_local_get(c, 0x8, 4);
    zh_local_set(c, 0x8, 4, dmg * 2);
}
zh_hook("Actor_attack", on_attack, NULL);
```

---

## 九、实用技巧

### 9.1 找特定值的槽位

**如果你想找"存着某个值的槽位"**：

```c
static void on_entry(ii_ctx* c, void* user) {
    (void)user;
    int n = ii_local_count(fn);
    for (int i = 0; i < n; i++) {
        uint16_t so; uint8_t ss; uint32_t io;
        if (ii_local_slot(fn, i, &so, &ss, &io) != 0) continue;
        int64_t v = zh_local_get(c, so, ss);
        if (v == 100) {   // 找值是 100 的
            LOG_I(LOG_LOCAL, "槽 sp+0x%x 是 100", so);
        }
    }
}
```

### 9.2 对比不同调用

```c
static void on_entry(ii_ctx* c, void* user) {
    (void)user;
    static int64_t last = 0;
    int64_t v = zh_local_get(c, 0x8, 4);
    if (last && v != last) {
        LOG_I(LOG_LOCAL, "值变化: %ld → %ld", last, v);
    }
    last = v;
}
```

---

## 下一步

- [字段访问](24-API-Field-Access)
- [返回值读写](28-API-Return-Value)
- [数据表查询](31-API-Data-Table)
