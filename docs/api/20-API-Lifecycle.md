# API：生命周期

初始化、状态查询、清理。

---

## 一、初始化

### 1.1 `zh_layer_init_default()`

```c
int zh_layer_init_default(void);
```

**作用**：用默认配置初始化引擎（幂等）。

**返回值**：

| 值 | 含义 |
|---|---|
| `ZH_OK` (0) | 成功 |
| `ZH_E_ALREADY` | 已初始化过 |
| `ZH_E_ENGINE` | 引擎层初始化失败 |
| `ZH_E_NO_BASE` | 找不到 `libminecraftpe.so` |
| `ZH_E_NO_DB` | 数据表为空 |

**做了什么**：

1. `zlog_init()` —— 日志
2. `ii_init()` —— 注册表 + watchdog 线程
3. `find_module_base("libminecraftpe.so")` —— 模块基址
4. `fingerprint()` —— 版本指纹
5. `ii_register_hint_pools()` —— 预注册 trampoline 池
6. 数据表自检

**示例**：

```c
int rc = zh_layer_init_default();
if (rc != ZH_OK && !zh_layer_ready()) {
    LOG_E(LOG_HOOK, "初始化失败 rc=%d", rc);
    return NULL;
}
```

**注意**：`ZH_E_ALREADY` 不算失败 —— 说明别的模块已经初始化过了。

---

### 1.2 `zh_layer_init()`

```c
typedef struct {
    const char* logPath;      // 日志路径（NULL = 默认）
    const char* cfiPath;      // CFI 表（NULL = 用静态数据）
    int         logLevel;     // 日志级别（-1 = 默认 INFO）
    int         registerPools;// 是否注册 poolhint（1 = 是）
} zh_layer_cfg;

int zh_layer_init(const zh_layer_cfg* cfg);
```

**用法**：

```c
zh_layer_cfg cfg = {
    .logPath       = "/data/data/com.netease.x19/cache/my.log",
    .cfiPath       = NULL,
    .logLevel      = ZLOG_INFO,
    .registerPools = 1,
};
zh_layer_init(&cfg);
```

**默认配置**：

```c
#define ZH_LAYER_CFG_DEFAULT { NULL, NULL, ZLOG_INFO, 1 }
```

---

### 1.3 `zh_layer_ready()`

```c
int zh_layer_ready(void);
```

**返回**：1 = 已就绪，0 = 未初始化。

**典型用法**：

```c
if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) {
    // 两个都失败才算真失败
}
```

---

## 二、状态查询

### 2.1 `zh_module_base()`

```c
unsigned long zh_module_base(void);
```

**返回**：`libminecraftpe.so` 的加载基址（0 = 未找到）。

**用途**：把数据表里的相对偏移转成绝对地址。

```c
unsigned long base = zh_module_base();
unsigned long abs_addr = base + 0x759b6c8;   // 数据表里的相对偏移
```

---

### 2.2 `zh_game_version()`

```c
uint64_t zh_game_version(void);
```

**返回**：从 `.so` 指纹算的版本标识（0 = 未知）。

**用途**：判断数据表是否匹配当前游戏版本。

```c
uint64_t ver = zh_game_version();
if (ver != EXPECTED_VERSION) {
    LOG_E(LOG_HOOK, "版本不匹配！数据表可能失效");
}
```

---

### 2.3 `zh_layer_status()`

```c
int zh_layer_status(char* buf, int len);
```

**作用**：把引擎状态格式化到一个字符串。

**输出示例**：
```
ready=1 base=0x7c3f637000 ver=0xbfdc5d91cc459fb0 hooks=3/262144 funcs=124 calls=449 slots=547
```

| 字段 | 含义 |
|---|---|
| `ready` | 是否就绪 |
| `base` | 模块基址 |
| `ver` | 版本指纹 |
| `hooks` | 已装站点数 / 上限 |
| `funcs` | 数据表函数数 |
| `calls` | 调用点数 |
| `slots` | 栈槽位数 |

**用法**：

```c
char st[128];
zh_layer_status(st, sizeof st);
LOG_I(LOG_HOOK, "%s", st);
```

---

### 2.4 `ii_count()`

```c
int ii_count(void);
```

**返回**：当前已安装的站点数。

**等价于**：`zh_layer_status` 里的 `hooks` 前半部分。

---

## 三、清理

### 3.1 `zh_unhook_all()`

```c
int zh_unhook_all(void);
```

**作用**：卸载所有站点，恢复原始指令。

**返回值**：成功卸载的数量。

**使用场景**：

```c
// 你的模块即将 dlclose 时，必须清理
// 否则站点指向已卸载的 shell → 野跳
__attribute__((destructor))
static void cleanup(void) {
    zh_unhook_all();
}
```

**重要**：**这是必须的**。如果你的 `.so` 被 `dlclose` 而站点还在，游戏下次调用该函数就会跳到已释放的内存。

---

### 3.2 `ii_uninstall_all()`

```c
void ii_uninstall_all(void);
```

**底层版本**，无返回值。

**内部用堆分配**（不是栈上 2 MB 数组）：

```c
void ii_uninstall_all(void) {
    int total = g_nSites;
    if (total <= 0) return;

    unsigned long* addrs = malloc(total * sizeof(unsigned long));
    if (!addrs) return;

    int n = 0;
    for (int i = 0; i < total; ++i) {
        ii_site* t = site_at(i);
        if (t) addrs[n++] = t->addr;
    }

    for (int i = 0; i < n; ++i) {
        ii_site* s = ii_find(addrs[i]);
        if (s) site_disable(s);
        zh_stop_wake_all();     // 清理暂停状态
        g_pauseDepth = 0;
        g_stopReq = 0;
    }

    for (int i = 0; i < n; ++i) {
        ii_site* s = ii_find(addrs[i]);
        if (s) site_release(s);
    }

    free(addrs);
}
```

> **历史教训**：早期版本用 `unsigned long addrs[II_MAX_SITES]`（262,144 × 8 = **2 MB 栈**），游戏工作线程栈只有 1~2 MB → 爆栈崩溃。

---

### 3.3 `zh_unhook_owner()`

```c
int zh_unhook_owner(const char* owner);
```

**作用**：按所有者名卸载所有站点。

**用法**：

```c
// 安装时标记
zh_set_owner("my_mod");
zh_hook("func1", cb1, NULL);
zh_hook("func2", cb2, NULL);

// 清理时按名卸载
zh_unhook_owner("my_mod");
```

**适合**：一个 Mod 有多个 hook，一次性清理。

---

## 四、完整生命周期示例

```c
#include <pthread.h>
#include <unistd.h>
#include "zh/zh_layer.h"
#include "zh/zlog.h"

static volatile int g_running = 1;

static void on_tick(ii_ctx* c, void* user) {
    (void)c; (void)user;
    // 轻量操作
}

static void* worker(void* arg) {
    (void)arg;

    // 1. 初始化
    int rc = zh_layer_init_default();
    if (rc != ZH_OK && !zh_layer_ready()) {
        return NULL;
    }

    // 2. 查状态
    char st[128];
    zh_layer_status(st, sizeof st);
    LOG_I(LOG_HOOK, "引擎状态: %s", st);

    // 3. 标记所有者
    zh_set_owner("my_mod");

    // 4. 安装
    if (zh_find("LocalPlayer_normalTick")) {
        zh_hook("LocalPlayer_normalTick", on_tick, NULL);
    }

    // 5. 主循环
    while (g_running) {
        sleep(5);
        LOG_I(LOG_HOOK, "hooks=%d", ii_count());
    }

    // 6. 清理（正常情况下走不到，但要写）
    zh_unhook_owner("my_mod");
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}

// ★ 库卸载时必须清理站点
__attribute__((destructor))
static void cleanup(void) {
    g_running = 0;
    usleep(100000);        // 给 worker 一点时间
    zh_unhook_owner("my_mod");
}
```

---

## 五、注意事项

| 事项 | 说明 |
|---|---|
| **初始化只能一次** | 多次调用返回 `ZH_E_ALREADY`，无害 |
| **不是游戏进程会失败** | `zh_layer_init` 返回 `ZH_E_NO_BASE` |
| **`constructor` 里不能阻塞** | 见 [常见陷阱](52-Guide-Pitfalls) |
| **destructor 必须清理** | 否则库卸载后站点野跳 |
| **多模块共享引擎** | 一个进程只初始化一次，多个 Mod 共用 |

---

## 下一步

- [函数查找](21-API-Find-Function)
- [Hook 安装](22-API-Hook-Install)
- [日志系统](32-API-Logging)
