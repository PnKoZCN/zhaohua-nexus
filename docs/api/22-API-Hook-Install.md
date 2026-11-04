# API：Hook 安装

本文讲所有安装类 API。

---

## 一、按函数名安装（推荐）

### 1.1 `zh_hook()`

```c
typedef void (*ii_callback)(ii_ctx* c, void* user);

int zh_hook(const char* funcName, ii_callback cb, void* user);
```

**作用**：在函数入口插桩。

**返回值**：

| 值 | 含义 |
|---|---|
| 0 | 成功 |
| `ZH_E_ARG` | 参数无效或函数不在数据表 |
| -2 | 已有站点（应该用 `ii_add_callback`） |
| -3 | 站点表满 |
| -5 | 指令不可插桩 |
| -10 | 写指令失败 |
| -15 | shell 分配失败 |
| -17 | 暂停线程失败（16 字节模式） |

**示例**：

```c
static void on_tick(ii_ctx* c, void* user) {
    (void)user;
    void* p_this = (void*)c->x[0];
    // ...
}

int rc = zh_hook("LocalPlayer_normalTick", on_tick, NULL);
if (rc != 0) {
    LOG_E(LOG_HOOK, "安装失败 rc=%d (%s)", rc, ii_strerror(rc));
    LOG_I(LOG_HOOK, "  %s", ii_strhint(rc));
}
```

---

### 1.2 `zh_hook_ex()`

```c
int zh_hook_ex(const char* funcName, ii_callback cb, void* user, int priority);
```

**和 `zh_hook` 相同，但可指定优先级。**

```c
zh_hook_ex("func", cb_high, NULL, 300);   // 先执行
zh_hook_ex("func", cb_low,  NULL, 100);   // 后执行
```

---

### 1.3 `zh_hook_ret()`

```c
int zh_hook_ret(const char* funcName, ii_callback cb, void* user);
```

**作用**：在函数 `ret` 前插桩（能读返回值）。

**要求**：数据表里有该函数的返回点记录（`retpoint` 表）。

**示例**：

```c
static void on_health(ii_ctx* c, void* user) {
    (void)user;
    float hp = zh_get_ret_float(c);
    if (hp > 0 && hp < 10.0f) {
        zh_set_ret_float(c, 20.0f);
    }
}

zh_hook_ret("Actor_getHealth", on_health, NULL);
```

**注意**：
- 一个函数可能有**多个返回点**（28 个是实测最多的）
- `zh_hook_ret` 默认挂第一个返回点
- 要挂所有返回点，用 `ii_install_at_return(func, idx, cb, user)`

---

### 1.4 `zh_hook_tail()`

```c
int zh_hook_tail(const char* funcName, ii_callback cb, void* user);
```

**作用**：在函数**最后一个** `ret` 前插桩。

**与 `zh_hook_ret` 的区别**：

| | `zh_hook_ret` | `zh_hook_tail` |
|---|---|---|
| 位置 | 第一个返回点 | 最后一个返回点 |
| 适用 | 单出口函数 | 多出口函数，总在最后 |

---

### 1.5 `zh_hook_light()`

```c
typedef void (*ii_light_callback)(ii_ctx_light* c, void* user);

int zh_hook_light(const char* funcName, ii_light_callback cb, void* user);
```

**作用**：用 LIGHT 模式安装（更轻量）。

**LIGHT vs FULL**：

| 模式 | 保存 | 栈帧 | 能力 |
|---|---|---|---|
| FULL | x0-x30, q0-q31, fpsr, fpcr, pstate | 0x320 | 全部 |
| LIGHT | x0-x7, q0-q1 | 96 B | 只读参数 |

**LIGHT 的限制**：
- 不能 cancel
- 不能改返回值
- 只能看前 8 个参数

**适用**：极高频函数（如 20 TPS 的 tick）。

```c
static void on_tick_light(ii_ctx_light* c, void* user) {
    (void)user;
    int64_t arg0 = c->x[0];
}

zh_hook_light("LocalPlayer_normalTick", on_tick_light, NULL);
```

---

### 1.6 `zh_hook_guarded()`

```c
int zh_hook_guarded(const char* funcName, ii_callback cb, void* user);
```

**作用**：带崩溃隔离的安装 —— 回调崩溃不会带崩游戏。

**代价**：每次回调有 `setjmp` 开销（约 20~50 ns）。

**示例**：

```c
static void risky(ii_ctx* c, void* user) {
    void* p = (void*)c->x[0];
    *(int*)0 = 1;   // 故意崩
}

zh_hook_guarded("SomeFunc", risky, NULL);

// 之后可以查
long n = zh_guard_caught();   // 崩溃被捕获的次数
LOG_W(LOG_HOOK, "已隔离 %ld 次崩溃", n);
```

**注意**：隔离的是**回调内部**的崩溃，不是 `ii_common` / trampoline 里的。

---

## 二、按地址安装（底层）

### 2.1 `ii_install()`

```c
int ii_install(unsigned long addr, ii_callback cb, void* user);
```

**作用**：直接按地址安装（不查表）。

**用途**：数据表里没有的函数，但你从别处知道了地址。

```c
unsigned long base = zh_module_base();
unsigned long addr = base + 0x759b6c8;   // 你自己算的偏移

int rc = ii_install(addr, cb, NULL);
```

**注意**：版本更新后这个偏移失效。

---

### 2.2 `ii_install_ex()`

```c
int ii_install_ex(unsigned long addr, ii_callback cb, void* user, int priority);
```

**同 `ii_install`，但可指定优先级。**

---

### 2.3 `ii_install_light()`

```c
int ii_install_light(unsigned long addr, ii_light_callback cb, void* user);
int ii_install_light_ex(unsigned long addr, ii_light_callback cb, void* user, int priority);
```

**按地址安装 LIGHT 模式。**

---

### 2.4 `ii_install_raw()`

```c
typedef void (*ii_detour)(void);

int ii_install_raw(unsigned long addr, ii_detour detour);
```

**作用**：直通模式 —— 完全替换函数，不构造 `ii_ctx`。

**特点**：
- 不保存寄存器（除了跳转本身）
- 你的 detour 拿到的是**原始现场**
- 需要自己处理返回

**示例**：

```c
static void my_detour(void) {
    // 直接是汇编层，看不到参数
    // 需要自己用内联汇编读写寄存器
}
```

**用途**：极高性能场景，或需要完全控制。

---

## 三、特殊位置安装

### 3.1 `ii_install_at_const()`

```c
int ii_install_at_const(const char* funcName, uint32_t value, int nth,
                        ii_callback cb, void* user);
```

**作用**：在函数内「编码了 `value` 的常量指令」处插桩。

**参数**：
- `value` —— 精确匹配的常量值
- `nth` —— 第几个（0 = 第一个）

**示例**：

```c
// 查有多少个常量 16
int cnt = zh_const_count("LocalPlayer_normalTick", 16);
LOG_I(LOG_HOOK, "有 %d 个常量 16", cnt);

// 在第 0 个处插桩
ii_install_at_const("LocalPlayer_normalTick", 16, 0, on_const, NULL);
```

**配合 `zh_modify_const`**（门面层）：

```c
zh_modify_const("LocalPlayer_normalTick", 16, 0, on_const, NULL);
```

---

### 3.2 `ii_install_at_return()`

```c
int ii_install_at_return(const char* funcName, int nth, ii_callback cb, void* user);
```

**作用**：在指定返回点插桩。

**`nth` = -1 表示"所有返回点"**。

```c
// 挂第一个
ii_install_at_return("Actor_getHealth", 0, cb, NULL);

// 挂所有
ii_install_at_return("Actor_getHealth", -1, cb, NULL);
```

---

### 3.3 `ii_install_at_tail()`

```c
int ii_install_at_tail(const char* funcName, ii_callback cb, void* user);
```

**作用**：在最后一个返回点插桩（`zh_hook_tail` 的底层）。

---

### 3.4 `ii_install_before_call()`

```c
int ii_install_before_call(const char* hostFunc, const char* targetFunc,
                           int nth, ii_callback cb, void* user);
```

**作用**：在 `hostFunc` 调用 `targetFunc` 的指令前插桩。

**示例**：

```c
// 在 getStackFromModel 调用 isNull 前插桩
ii_install_before_call("ContainerScreenController_getStackFromModel",
                       "ItemStack_isNull", 0, cb, NULL);
```

---

## 四、卸载

### 4.1 `zh_unhook()`

```c
int zh_unhook(const char* funcName);
```

**作用**：卸载函数入口和所有返回点的站点。

**返回值**：0 = 成功，负数 = 失败。

**示例**：

```c
int rc = zh_unhook("LocalPlayer_normalTick");
if (rc != 0) {
    LOG_W(LOG_HOOK, "卸载失败 rc=%d", rc);
}
```

**注意**：`zh_unhook` 会尝试卸载入口 + 所有返回点。

---

### 4.2 `ii_uninstall()`

```c
int ii_uninstall(unsigned long addr);
```

**按地址卸载**（入口或返回点都行）。

---

### 4.3 `zh_unhook_all()`

```c
int zh_unhook_all(void);
```

**卸载所有站点。**

**必须在库卸载前调用**：

```c
__attribute__((destructor))
static void cleanup(void) {
    zh_unhook_all();
}
```

---

## 五、启停控制

### 5.1 `ii_set_enabled()`

```c
int ii_set_enabled(unsigned long addr, int on);
```

**作用**：标记为"待启用/待禁用"（不立即动代码）。

**配合 `ii_apply_pending()`**：

```c
// 标记 100 个站点
for (...) ii_set_enabled(addr[i], 0);

// 一次性应用（只暂停一次线程）
ii_apply_pending();
```

**优势**：批量操作时只暂停一次线程，而不是每个站点都暂停。

---

### 5.2 `ii_apply_pending()`

```c
int ii_apply_pending(void);
```

**作用**：应用所有待办（启用/禁用）。

**返回值**：成功数量。

**内部优化**：

```c
int needStop = 0;
for (int i = 0; i < n; ++i)
    if (list[i]->wideMode) { needStop = 1; break; }

int stopped = needStop ? suspend_others() : 0;
// ... 应用
if (needStop) resume_others();
```

**只有 16 字节模式才暂停线程**，4 字节路径完全无锁。

---

### 5.3 `ii_is_enabled()` / `ii_pending_count()`

```c
int ii_is_enabled(unsigned long addr);
int ii_pending_count(void);
```

**查询状态。**

---

## 六、安装返回码速查

```c
const char* ii_strerror(int rc);   // 错误描述
const char* ii_strhint(int rc);    // 修复建议
```

| rc | 含义 | 建议 |
|---|---|---|
| 0 | 成功 | — |
| -1 | 参数无效 | 检查 addr / cb 非 NULL |
| -2 | 已有站点 | 用 `ii_add_callback` 追加 |
| -3 | 站点表满 | 卸载不用的 |
| -4 | 引擎未初始化 | 先 `zh_layer_init` |
| -5 | 指令不可插桩 | 换函数 |
| -6 | mmap 失败 | 内存不足 |
| -7 | 无法覆盖 16 字节 | 换函数 |
| -9 | 内存分配失败 | 内存不足 |
| -10 | 写指令失败 | mprotect 失败 |
| -11 | b 编码失败 | 距离超 ±128MB |
| -13 | 后续指令不干净 | 罕见，换函数 |
| -15 | shell 分配失败 | 池满 |
| -17 | 暂停线程失败 | 有 D 状态线程 |

---

## 七、完整示例

```c
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include "zh/zh_layer.h"
#include "zh/zlog.h"

// ═══ 回调 ═══
static volatile unsigned long g_ticks = 0;
static volatile unsigned long g_health_fixed = 0;

static void on_tick(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);
}

static void on_health(ii_ctx* c, void* user) {
    (void)user;
    float hp = zh_get_ret_float(c);
    if (hp > 0.0f && hp < 10.0f) {
        zh_set_ret_float(c, 20.0f);
        __atomic_add_fetch(&g_health_fixed, 1, __ATOMIC_RELAXED);
    }
}

// ═══ 工作线程 ═══
static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;

    zh_set_owner("demo_mod");

    // 入口 hook
    int r1 = zh_hook("LocalPlayer_normalTick", on_tick, NULL);
    LOG_I(LOG_HOOK, "tick hook rc=%d", r1);

    // 返回点 hook
    int r2 = zh_hook_ret("Actor_getHealth", on_health, NULL);
    LOG_I(LOG_HOOK, "health hook rc=%d", r2);

    // 主循环
    while (1) {
        sleep(5);
        LOG_I(LOG_HOOK, "ticks=%lu hp_fixed=%lu hooks=%d guard=%ld",
              __atomic_load_n(&g_ticks, __ATOMIC_RELAXED),
              __atomic_load_n(&g_health_fixed, __ATOMIC_RELAXED),
              ii_count(),
              zh_guard_caught());
    }

    zh_unhook_owner("demo_mod");
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}

__attribute__((destructor))
static void cleanup(void) {
    zh_unhook_owner("demo_mod");
}
```

---

## 下一步

- [多回调链](23-API-Multi-Callback)
- [返回值读写](28-API-Return-Value)
- [崩溃隔离](30-API-Crash-Isolation)
- [错误码](40-Reference-Error-Codes)
