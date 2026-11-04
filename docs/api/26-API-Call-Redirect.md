# API：调用重定向

把一个函数调用改成跳到你的实现 —— 对标 Mixin `@Redirect`。

---

## 一、核心 API

### 1.1 `zh_redirect()`

```c
typedef void* (*zh_redirect_fn)(ii_ctx* c, void* user);

int zh_redirect(const char* hostFunc, const char* targetFunc,
                zh_redirect_fn cb, void* user);
```

**作用**：在 `hostFunc` 调用 `targetFunc` 的地方插桩，回调决定跳转到哪。

**回调返回值**：
- `NULL` —— 走原目标（不改变行为）
- 非 `NULL` —— 跳到该地址

**示例**：

```c
static void* on_call(ii_ctx* c, void* user) {
    (void)user;

    // 读参数（原调用的参数在 x0..x7）
    void* arg0 = (void*)c->x[0];

    // 返回 NULL = 走原目标
    if (!should_override(arg0)) return NULL;

    // 返回你的实现
    return (void*)my_replacement;
}

zh_redirect("ContainerScreenController_getStackFromModel",
            "ItemStack_isNull",
            on_call, NULL);
```

---

### 1.2 `ii_redirect_name()`

```c
int ii_redirect_name(const char* hostFunc, const char* targetFunc, int nth,
                     ii_redirect_cb cb, void* user);
```

**`zh_redirect` 的底层版本**，可指定第几个调用点。

**`nth = -1`** 表示第一个（`zh_redirect` 传的就是 -1）。

---

### 1.3 `ii_redirect_call()`

```c
int ii_redirect_call(unsigned long callSiteAddr, unsigned long origTarget,
                     ii_redirect_cb cb, void* user);
```

**按地址重定向**（最底层）。

**要求**：`callSiteAddr` 处的指令必须是 `bl`。

---

### 1.4 `ii_redirect_orig_target()`

```c
unsigned long ii_redirect_orig_target(unsigned long callSiteAddr);
```

**作用**：查某调用点的原始目标地址。

**用途**：在回调里判断"原本要调用谁"。

```c
static void* on_call(ii_ctx* c, void* user) {
    unsigned long orig = ii_redirect_orig_target(c->pc);
    LOG_I(LOG_HOOK, "原本要调用 0x%lx", orig);
    return NULL;
}
```

---

## 二、重定向的原理

### 2.1 原指令

```asm
hostFunc:
    ...
    bl   targetFunc          ← 这条被改写
    ...
```

### 2.2 改写后

```asm
hostFunc:
    ...
    b    shell               ← 4 字节原子写
    ...
```

### 2.3 shell（专门的重定向版本）

```asm
shell:
    stp  x16, x17, [sp, #-16]!
    movz x17, #sid
    ldr  x16, #8
    br   x16
    .quad ii_redirect_common    ← 注意不是 ii_common
```

### 2.4 `ii_redirect_common`

```asm
ii_redirect_common:
    sub  sp, sp, #0x330
    stp  x0, x1, [sp, #0x000]
    ...（保存 x0-x15, q0-q7）
    mov  x0, sp
    mov  x1, x17
    bl   ii_redirect_dispatch    ← 调回调
    str  x0, [sp, #0x320]        ← 保存目标地址
    ...（恢复 q0-q7, x0-x15）
    ldr  x16, [sp, #0x320]       ← 目标
    ldr  x9,  [sp, #0x100]       ← 返回地址 = callSite+4
    mov  x30, x9
    add  sp, sp, #0x330
    add  sp, sp, #0x10           ← 补回 shell 压的
    cbz  x16, 1f                 ← NULL 兜底
    br   x16
1:  ret
```

**关键点**：
- 模拟了 `bl` 的语义（设置 `x30` = callSite+4）
- 目标函数 `ret` 后回到 callSite+4

---

## 三、实测案例（t12）

### 3.1 目标

**`ContainerScreenController_getStackFromModel` 调用 `ItemStack_isNull` → 改成我们的实现。**

**数据表记录**：

```
调用点 +0xa0    目标 +0xd795008
```

### 3.2 代码

```c
static volatile unsigned long g_hits = 0;
static volatile unsigned long g_orig_target = 0;
static volatile unsigned long g_call_site = 0;

static void* on_redirect(ii_ctx* c, void* user) {
    (void)user;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);

    static int once = 0;
    if (__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED) == 0) {
        __atomic_store_n(&g_orig_target, ii_redirect_orig_target(c->pc),
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_call_site, c->pc, __ATOMIC_RELAXED);
    }

    return NULL;   // 走原目标，不改变行为
}

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;

    // 1. 先算调用点（卸载时要用）
    unsigned long call_addr = 0;
    int car = ii_call_site_addr("ContainerScreenController_getStackFromModel",
                                "ItemStack_isNull", 0, &call_addr);
    LOG_I(LOG_HOOK, "调用点 rc=%d addr=0x%lx", car, call_addr);

    // 2. 安装
    int rc = zh_redirect("ContainerScreenController_getStackFromModel",
                         "ItemStack_isNull", on_redirect, NULL);
    LOG_I(LOG_HOOK, "redirect rc=%d", rc);

    // 3. 观察（需要开源码里打开箱子/背包才会触发）
    sleep(6);
    LOG_I(LOG_HOOK, "hits=%lu", __atomic_load_n(&g_hits, __ATOMIC_RELAXED));

    // 4. 卸载（必须！）
    if (call_addr) {
        ii_uninstall(call_addr);
    }
    zh_unhook_all();   // 双保险

    while (1) sleep(5);
    return NULL;
}
```

### 3.3 结果

```
OK   两个函数都在数据表里    host=0x7c45db46ac target=0x7c4cdcc008
OK   ii_call_site_addr 定位调用点 rc=0 addr=0x7c45db474c
OK   zh_redirect 安装        rc=0
（未开背包，hits=0，符合预期）
OK   ii_uninstall 撤销重定向   rc=0
OK   重定向站点已移除         已清除
OK   zh_unhook_all 兜底清理   rc=0
```

**进程存活 20 秒，无崩溃。**

---

## 四、必须卸载（极其重要）

### 4.1 不卸载会怎样

**`zh_redirect` 改写了 `hostFunc` 里的 `bl` 指令。**

**如果库 `dlclose` 而站点还在：**

```
hostFunc 的 bl → shell（在已卸载的内存里）
下次调用 hostFunc → 跳到野内存 → 闪退，无 tombstone
```

### 4.2 正确做法

```c
__attribute__((destructor))
static void cleanup(void) {
    // 方式 1：按调用点卸载
    if (g_call_site) ii_uninstall(g_call_site);

    // 方式 2：兜底
    zh_unhook_all();
}
```

**或者更保险：两个都做。**

```c
static void safe_cleanup(void) {
    // 先按地址
    unsigned long a = 0;
    if (ii_call_site_addr(host, target, 0, &a) == 0 && a) {
        ii_uninstall(a);
    }
    // 再全清
    zh_unhook_all();
}
```

---

## 五、查调用点

### 5.1 `ii_call_site_addr()`

```c
int ii_call_site_addr(const char* hostFunc, const char* targetFunc,
                      int nth, unsigned long* outAddr);
```

**作用**：查 `hostFunc` 调用 `targetFunc` 的指令地址。

**返回**：0 = 成功，-1 = 参数错，-3 = host 不在表，-4 = 找不到该组合。

**注意**：`nth < 0` 会被当作 0 处理。

---

### 5.2 `ii_call_site_count()`

```c
int ii_call_site_count(const char* hostFunc, const char* targetFunc);
```

**返回**：该组合的调用点数量。

```c
int n = ii_call_site_count("host", "target");
LOG_I(LOG_HOOK, "有 %d 个调用点", n);

// 遍历
for (int i = 0; i < n; i++) {
    unsigned long a = 0;
    ii_call_site_addr("host", "target", i, &a);
    LOG_I(LOG_HOOK, "  [%d] 0x%lx", i, a);
}
```

---

### 5.3 枚举所有调用点

```c
static void dump_calls(const char* host) {
    unsigned long host_addr = zh_find(host);
    if (!host_addr) return;

    LOG_I(LOG_HOOK, "=== %s 的调用 ===", host);

    int n = zh_tbl_count(ZH_TBL_BLCALL);
    for (int i = 0; i < n; i++) {
        const char *bf, *bt; uint32_t boff, btoff;
        if (zh_tbl_blcall(i, &bf, &boff, &bt, &btoff) != 0) continue;
        if (!bf || strcmp(bf, host) != 0) continue;

        LOG_I(LOG_HOOK, "  +0x%-6x → %s (+0x%x)",
              boff, bt ? bt : "?", btoff);
    }
}
```

---

## 六、参数与返回值

### 6.1 读参数

```c
static void* on_call(ii_ctx* c, void* user) {
    // 原调用的参数在 x0..x7
    void* p0 = (void*)c->x[0];
    int32_t a0 = (int32_t)c->x[0];
    int32_t a1 = (int32_t)c->x[1];

    // 可以修改参数（会传给目标函数）
    c->x[0] = (uint64_t)new_p0;

    return NULL;
}
```

### 6.2 改返回值

**重定向的回调只决定"跳到哪"，不直接给返回值。**

**要改返回值，有两个方法**：

**方法 A：返回自己的实现**

```c
static bool my_is_empty(void* p_stack) {
    return false;   // 永不空
}

static void* on_call(ii_ctx* c, void* user) {
    (void)c; (void)user;
    return (void*)my_is_empty;
}
```

**方法 B：用 `zh_hook_ret` 改**

```c
// 但这改的是"被调用函数"的返回，不是调用点
zh_hook_ret("ItemStack_isNull", on_ret, NULL);
```

---

## 七、常见用法

### 7.1 完全替换某调用

```c
static void* on_call(ii_ctx* c, void* user) {
    (void)c; (void)user;
    return (void*)my_impl;    // 总是走自己的
}
```

### 7.2 条件替换

```c
static void* on_call(ii_ctx* c, void* user) {
    (void)user;
    void* p = (void*)c->x[0];
    if (should_use_mine(p)) return (void*)my_impl;
    return NULL;   // 否则走原样
}
```

### 7.3 只观察不干预

```c
static volatile unsigned long g_calls = 0;

static void* on_call(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_add_fetch(&g_calls, 1, __ATOMIC_RELAXED);
    return NULL;   // 永远走原样
}
```

### 7.4 改参数后走原目标

```c
static void* on_call(ii_ctx* c, void* user) {
    (void)user;
    // 把第一个参数改掉
    c->x[0] = (uint64_t)new_value;
    return NULL;   // 走原目标，但用新参数
}
```

---

## 八、16 字节模式（兜底）

**如果 ±128 MB 内没有 shell 空间**，退回 16 字节改写：

```asm
addr+0x00:  ldr  x16, #8       ; 0x58000050
addr+0x04:  br   x16           ; 0xD61F0200
addr+0x08:  .quad shell        ; shell 可以在任意远处
（覆盖 4 条指令）
```

**代价**：
- 要搬走 3 条原指令到 trampoline
- 非原子写，**必须暂停所有线程**

**trampoline 布局**：

```asm
tramp:
    <原指令 A>
    <原指令 B>
    <原指令 C>
    ldr  x16, #8
    br   x16
    .quad addr+16       ; 跳回
```

**`dispatch` 里的 `c->pc` 指向 trampoline**（不是 callSite+4），这样目标函数 `ret` 后会执行 A/B/C 再跳回。

---

## 九、注意事项

| 事项 | 说明 |
|---|---|
| **必须卸载** | 否则库卸载后野跳 |
| 调用点必须是 `bl` | 不是的话返回 -5 |
| 目标必须是可执行地址 | 数据表里已验证 |
| 无效目标名 | 返回 `ZH_E_ARG` |
| 回调里不能阻塞 | 同一铁律 |

---

## 下一步

- [栈局部变量](27-API-Stack-Local)
- [返回值读写](28-API-Return-Value)
- [常见陷阱](52-Guide-Pitfalls)
