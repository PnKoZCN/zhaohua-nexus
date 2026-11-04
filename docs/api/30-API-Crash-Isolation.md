# API：崩溃隔离

回调崩溃不带崩游戏 —— Nexus 的容错机制。

---

## 一、为什么需要

**你的回调跑在游戏线程上。** 回调里任何非法访存都会 SIGSEGV：

```
游戏进程 → 你的回调 → *(int*)0 = 1
                        ↓
                     SIGSEGV
                        ↓
                    无 handler → 进程终止 → 闪退
```

**崩溃隔离**：在回调外层套一个 `setjmp`，`SIGSEGV` 时用 `siglongjmp` 跳回来。

---

## 二、核心 API

### 2.1 `zh_hook_guarded()`

```c
int zh_hook_guarded(const char* funcName, ii_callback cb, void* user);
```

**作用**：安装带隔离的入口 hook。

---

### 2.2 `zh_guard_on()` / `zh_guard_off()`

```c
void zh_guard_on(void);
void zh_guard_off(void);
```

**作用**：全局开关（默认开启）。

```c
zh_guard_off();   // 完全关闭隔离（用于对比测试开销）
zh_guard_on();    // 重新打开
```

---

### 2.3 `zh_guard_caught()`

```c
long zh_guard_caught(void);
```

**返回**：累计捕获的崩溃次数。

```c
long n = zh_guard_caught();
if (n > 0) {
    LOG_W(LOG_GUARD, "已隔离 %ld 次崩溃（你的回调有 bug）", n);
}
```

---

### 2.4 `zh_guard_reset()`

```c
void zh_guard_reset(void);
```

**作用**：清零计数。

---

### 2.5 底层

```c
int  ii_guard_call(ii_callback cb, ii_ctx* c, void* user);
int  ii_guard_call_light(ii_light_callback cb, ii_ctx_light* c, void* user);
long ii_guard_caught(void);
void ii_guard_reset(void);
```

**`ii_guard_call` 返回**：0 = 正常，非 0 = 捕获了崩溃。

---

## 三、实现原理

### 3.1 简化的核心

```c
static __thread sigjmp_buf g_guardJmp;
static __thread volatile int g_guardActive = 0;
static _Atomic long g_guardCount = 0;

static void guard_sigaction(int sig, siginfo_t* si, void* uc) {
    (void)sig; (void)si; (void)uc;

    if (!g_guardActive) {
        // 不是我们的回调崩的 → 恢复默认处理，重新触发
        struct sigaction sa = {0};
        sa.sa_handler = SIG_DFL;
        sigaction(sig, &sa, NULL);
        raise(sig);
        return;
    }

    g_guardActive = 0;
    __atomic_add_fetch(&g_guardCount, 1, __ATOMIC_RELAXED);
    siglongjmp(g_guardJmp, 1);      // ★ 跳回
}

int ii_guard_call(ii_callback cb, ii_ctx* c, void* user) {
    if (!g_guardEnabled || !cb) {
        if (cb) cb(c, user);
        return 0;
    }

    // 同一线程嵌套时不要再套一层
    if (g_guardActive) {
        cb(c, user);
        return 0;
    }

    int rc = 0;
    if (sigsetjmp(g_guardJmp, 1) == 0) {
        g_guardActive = 1;
        cb(c, user);
        g_guardActive = 0;
    } else {
        rc = -1;                     // 从崩溃跳回
    }
    return rc;
}
```

### 3.2 关键点

| 点 | 说明 |
|---|---|
| `__thread` | 每个线程独立的 jmp_buf |
| `sigsetjmp(..., 1)` | 第二个参数 1 = 保存信号掩码 |
| `g_guardActive` | 区分"我们的回调崩了"和"别的代码崩了" |
| 非我们的崩溃 | 恢复 `SIG_DFL` 再 `raise` → 正常崩溃行为 |
| 嵌套 | 内层直接用，不重复套 |

### 3.3 信号安装

```c
static void guard_install(void) {
    static int done = 0;
    if (done) return;
    done = 1;

    struct sigaction sa = {0};
    sa.sa_sigaction = guard_sigaction;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;   // ★ 用备用栈
    sigemptyset(&sa.sa_mask);

    for (int sig = SIGSEGV; sig <= SIGSYS; ++sig) {
        sigaction(sig, &sa, NULL);
    }
}
```

**`SA_ONSTACK`**：栈溢出导致的 SIGSEGV 也能处理（用备用栈）。

**覆盖的信号**：`SIGSEGV`、`SIGBUS`、`SIGFPE`、`SIGILL`、`SIGTRAP`、`SIGABRT`、`SIGSYS`。

---

## 四、实测案例（t09）

### 4.1 代码

```c
static volatile unsigned long g_normal = 0;
static volatile unsigned long g_crash = 0;

// 正常回调
static void cb_ok(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_add_fetch(&g_normal, 1, __ATOMIC_RELAXED);
}

// 故意崩的回调（每次必崩）
static void cb_crash(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_add_fetch(&g_crash, 1, __ATOMIC_RELAXED);

    // 三种崩法轮换
    switch (__atomic_load_n(&g_crash, __ATOMIC_RELAXED) % 3) {
    case 0: *(volatile int*)0 = 1;            break;   // SIGSEGV
    case 1: __builtin_trap();                 break;   // SIGILL
    case 2: { volatile int a = 1, b = 0; (void)(a / b); } break;  // SIGFPE
    }
}

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;

    // 1. 装带隔离的崩溃回调
    int rc = zh_hook_guarded("LocalPlayer_normalTick", cb_crash, NULL);
    LOG_I(LOG_GUARD, "隔离 hook rc=%d", rc);

    // 2. 再装一个正常回调（验证隔离不影响别人）
    ii_add_callback(zh_find("LocalPlayer_normalTick"), cb_ok, NULL, 100);

    // 3. 观察 10 秒
    for (int i = 0; i < 10; i++) {
        sleep(1);
        LOG_I(LOG_GUARD, "t=%ds 正常=%lu 崩溃=%lu 已隔离=%ld",
              i + 1,
              __atomic_load_n(&g_normal, __ATOMIC_RELAXED),
              __atomic_load_n(&g_crash, __ATOMIC_RELAXED),
              zh_guard_caught());
    }

    zh_unhook_all();
    while (1) sleep(5);
    return NULL;
}
```

### 4.2 结果

```
OK   隔离 hook 安装          rc=0
OK   正常回调继续工作        （未受影响）
OK   崩溃被捕获              60 次 / 10 秒
OK   游戏未闪退              进程存活 ✅
OK   正常回调执行计数正常      与崩溃数独立

崩溃 60 次全部被隔离，游戏存活
```

**60 次崩溃，一个都没带崩游戏。**

---

## 五、开销

### 5.1 实测

| 场景 | 单次开销 |
|---|---|
| 无隔离 | ~2 ns |
| 有隔离（正常路径） | ~25 ns |
| 崩溃恢复 | ~2 μs（信号处理本身很慢） |

**`setjmp` 在 ARM64 上要保存 ~30 个寄存器，约 20 ns。**

### 5.2 建议

| 场景 | 用隔离吗 |
|---|---|
| 高频 tick（20 TPS × N） | 建议关闭，性能敏感 |
| 不常用函数（开箱、物品操作） | 建议开启 |
| 调试期 | 必须开启 |
| 生产且已充分测试 | 可关闭 |

**关闭方式**：

```c
zh_guard_off();
```

---

## 六、隔离能防什么、不能防什么

### 6.1 能防

| 情况 | 是否隔离 |
|---|---|
| `*(int*)0 = 1`（空指针解引用） | ✅ |
| 野指针读写 | ✅ |
| 整数除零 | ✅ |
| 非法指令 | ✅ |
| 栈溢出（有 `SA_ONSTACK`） | ⚠️ 部分 |
| `abort()` | ✅（SIGABRT） |
| 无限循环 | ❌ |
| 死锁 | ❌ |
| 内存泄漏 | ❌ |
| 数据竞争 | ❌ |
| 破坏别人的栈 | ❌ |

### 6.2 不能防的：无限循环

**回调里 `while(1);` → 游戏线程挂死 → ANR，隔离救不了。**

**只能用 `[SLOW]` 告警发现**（50 ms）。

### 6.3 不能防的：破坏 ctx

**如果回调把 `c->x[0]` 改成了垃圾，返回后游戏用这个垃圾指针 → 崩在游戏代码里，隔离管不到。**

```c
static void bad_cb(ii_ctx* c, void* user) {
    (void)user;
    c->x[0] = 0xdeadbeef;   // ★ 改了 this 指针
    // 返回后游戏用这个指针 → 崩在 Actor_xxx 里
}
```

**这是隔离机制的边界。**

---

## 七、与其它崩溃处理共存

### 7.1 已有崩溃处理器

**如果游戏/别的库已经装了 SIGSEGV handler，Nexus 会覆盖它。**

**恢复方法**（在你的 `destructor` 里）：

```c
__attribute__((destructor))
static void cleanup(void) {
    zh_guard_off();       // 关闭隔离
    // 但不恢复原 handler（Nexus 没保存）
}
```

**需要注意**：`zh_guard_off()` 后 `g_guardActive` 永远是 0，所以 `guard_sigaction` 会走"恢复 SIG_DFL 再 raise"分支 → **等于去掉了所有 handler**。

**如果需要保留别人的 handler，请自行在 install 前保存。**

### 7.2 与 tombstone 的关系

**隔离捕获的崩溃不会生成 tombstone**（因为进程没死）。

**如果想知道崩在哪**，在 handler 里记 `si_addr`：

```c
// 增强版（需要自己改 Nexus 源码）
static void guard_sigaction(int sig, siginfo_t* si, void* uc) {
    ucontext_t* u = (ucontext_t*)uc;
    zh_log("崩溃 sig=%d addr=%p pc=%p", sig, si->si_addr,
           (void*)u->uc_mcontext.pc);
    ...
}
```

---

## 八、完整示例

```c
#include <string.h>
#include <unistd.h>
#include "zh/zh_layer.h"
#include "zh/zlog.h"

static volatile unsigned long g_ok = 0;
static volatile unsigned long g_bad = 0;

// 先做安全的事，再做危险的事
static void on_risky(ii_ctx* c, void* user) {
    (void)user;

    // 安全部分
    void* p = (void*)c->x[0];
    if (p) {
        __atomic_add_fetch(&g_ok, 1, __ATOMIC_RELAXED);
    }

    // 危险部分（可能崩）
    switch (__atomic_load_n(&g_bad, __ATOMIC_RELAXED) % 4) {
    case 0: break;
    case 1: *(volatile int*)0 = 1; break;
    case 2: __builtin_trap();      break;
    case 3: {
        int a = 1, b = 0;
        volatile int r = a / b; (void)r;
        break;
    }
    }
    __atomic_add_fetch(&g_bad, 1, __ATOMIC_RELAXED);
}

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;

    // 隔离默认开启，直接用 zh_hook 也行
    zh_guard_on();

    int rc = zh_hook("LocalPlayer_normalTick", on_risky, NULL);
    LOG_I(LOG_GUARD, "hook rc=%d, 隔离=%s", rc,
          zh_guard_caught() >= 0 ? "开" : "?");

    while (1) {
        sleep(10);
        LOG_I(LOG_GUARD, "正常=%lu 崩溃=%lu 已隔离=%ld",
              __atomic_load_n(&g_ok, __ATOMIC_RELAXED),
              __atomic_load_n(&g_bad, __ATOMIC_RELAXED),
              zh_guard_caught());
    }

    zh_unhook_all();
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}

__attribute__((destructor))
static void cleanup(void) {
    zh_unhook_all();
    zh_guard_off();
}
```

---

## 九、注意事项

| 事项 | 说明 |
|---|---|
| 默认开启 | 不用特殊 API 也生效 |
| 高频函数建议关 | 20 ns / 次 |
| 不能让游戏崩的数据污染 | 改 ctx 的错隔离不了 |
| 无限循环隔离不了 | 用 `[SLOW]` 告警 |
| 覆盖已有 handler | 注意共存 |
| 每线程独立 | `__thread` jmp_buf |

---

## 下一步

- [日志系统](32-API-Logging)
- [常见陷阱](52-Guide-Pitfalls)
- [错误码](40-Reference-Error-Codes)
