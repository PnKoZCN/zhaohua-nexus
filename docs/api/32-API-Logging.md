# API：日志

分级日志，写到文件，不阻塞游戏线程。

---

## 一、核心 API

### 1.1 `zlog_init()`

```c
int zlog_init(const char* path, int level);
```

**参数**：
- `path` —— 日志文件路径（`NULL` = 默认）
- `level` —— 级别（`-1` = 默认 INFO）

**默认路径**：

```c
#define ZLOG_DEFAULT_PATH "/data/data/com.netease.x19/files/zhprobe/zhaohua.log"
```

**示例**：

```c
zlog_init("/data/data/com.netease.x19/cache/my.log", ZLOG_DEBUG);
```

**返回**：0 = 成功。

---

### 1.2 级别常量

```c
enum {
    ZLOG_OFF   = 0,
    ZLOG_ERROR = 1,
    ZLOG_WARN  = 2,
    ZLOG_INFO  = 3,
    ZLOG_DEBUG = 4,
    ZLOG_TRACE = 5,
};
```

| 级别 | 用途 |
|---|---|
| `ERROR` | 必然出问题 |
| `WARN` | 可能有问题的迹象 |
| `INFO` | 关键流程节点 |
| `DEBUG` | 详细流程 |
| `TRACE` | 每次调用 |

---

### 1.3 `zlog_set_level()`

```c
void zlog_set_level(int level);
int  zlog_level(void);
```

**运行时调整**：

```c
zlog_set_level(ZLOG_DEBUG);   // 打开详细日志
// ... 复现问题 ...
zlog_set_level(ZLOG_INFO);    // 关掉
```

---

### 1.4 日志宏

```c
#define LOG_E(tag, fmt, ...)
#define LOG_W(tag, fmt, ...)
#define LOG_I(tag, fmt, ...)
#define LOG_D(tag, fmt, ...)
#define LOG_T(tag, fmt, ...)
```

**`tag` 是模块标签**（见下方常量）。

**示例**：

```c
LOG_I(LOG_HOOK,  "安装成功 addr=0x%lx", addr);
LOG_W(LOG_POOL,  "池 %d 剩余 %d 槽", idx, left);
LOG_E(LOG_GUARD, "捕获崩溃 pc=0x%lx", pc);
LOG_D(LOG_LOCAL, "槽 sp+0x%x = %ld", off, val);
```

---

## 二、模块标签

```c
#define LOG_HOOK    "HOOK"
#define LOG_POOL    "POOL"
#define LOG_LOCAL   "LOCAL"
#define LOG_MULTI   "MULTI"
#define LOG_GUARD   "GUARD"
#define LOG_TBL     "TBL"
#define LOG_INIT    "INIT"
#define LOG_REDIR   "REDIR"
```

**标签只影响日志文本格式**，不影响功能。

---

## 三、输出格式

```
[2026-11-05 06:12:33.421] [I] [HOOK] 安装成功 addr=0x7c45db46ac
 │                       │   │      │
 │                       │   │      └─ 你的消息
 │                       │   └─ 标签
 │                       └─ 级别字母
 └─ 时间戳（毫秒精度）
```

**级别字母**：

| 级别 | 字母 |
|---|---|
| ERROR | `E` |
| WARN | `W` |
| INFO | `I` |
| DEBUG | `D` |
| TRACE | `T` |

---

## 四、内部实现

### 4.1 不阻塞调用线程

**日志写入用独立的缓冲区 + 定期 flush**：

```c
static char g_buf[64 * 1024];
static size_t g_used = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void zlog_write(int level, const char* tag, const char* fmt, ...) {
    if (level > g_level) return;

    char line[1024];
    int n = format_line(line, sizeof line, level, tag, fmt, ...);
    if (n <= 0) return;

    pthread_mutex_lock(&g_lock);
    if (g_used + n > sizeof g_buf) {
        flush_locked();          // 缓冲满了才写盘
    }
    memcpy(g_buf + g_used, line, n);
    g_used += n;
    pthread_mutex_unlock(&g_lock);
}
```

**关键**：`pthread_mutex_lock` 很短（只在内存里 memcpy），写盘是偶尔的。

**实测**：`LOG_I` 一次约 **200 ns**。

### 4.2 为什么不能在回调里打太多日志

**每行日志 ~200 ns，但写盘时会持锁。**

如果回调里每帧打 10 行日志：

```
20 TPS × 10 = 200 行/秒
= 200 × 200 ns = 40 μs/秒
```

**可接受。**

但如果 10 个 hook 各打 10 行：

```
2000 行/秒 = 400 μs/秒
```

**仍然可接受，但日志文件会很大（几 MB/分钟）。**

### 4.3 定期 flush

```c
static void* flusher(void* arg) {
    (void)arg;
    while (g_running) {
        usleep(500 * 1000);      // 500 ms
        pthread_mutex_lock(&g_lock);
        flush_locked();
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}
```

**500 ms 一次**，进程崩溃最多丢 500 ms 日志。

---

## 五、日志路径

### 5.1 默认路径

```
/data/data/com.netease.x19/files/zhprobe/zhaohua.log
```

**这个目录游戏有写权限。**

### 5.2 查看日志

```bash
adb shell su -c "tail -f /data/data/com.netease.x19/files/zhprobe/zhaohua.log"
```

**或**：

```bash
adb shell su -c "cat /data/data/com.netease.x19/files/zhprobe/zhaohua.log | grep '\[E\]'"
```

### 5.3 日志轮转

**没有自动轮转。**

**手动清理**：

```bash
adb shell su -c "> /data/data/com.netease.x19/files/zhprobe/zhaohua.log"
```

**或者限制大小**（自己检查）：

```c
static void check_log_size(void) {
    struct stat st;
    if (stat(ZLOG_DEFAULT_PATH, &st) == 0 && st.st_size > 8 * 1024 * 1024) {
        // 超过 8 MB，截断
        int fd = open(ZLOG_DEFAULT_PATH, O_WRONLY | O_TRUNC);
        if (fd >= 0) close(fd);
    }
}
```

---

## 六、引擎自己的日志

**引擎内部也会打日志**，标签为：

| 标签 | 内容 |
|---|---|
| `[ENGINE]` | 初始化、安装、卸载 |
| `[SLOW]` | 回调超时告警 |
| `[POOL DEAD]` | 池失效 |
| `[RECOVER]` | 池恢复 |
| `[WD]` | watchdog |

**示例**：

```
[2026-11-05 06:12:30.100] [I] [ENGINE] zh_layer 初始化完成 base=0x7c3f637000
[2026-11-05 06:12:30.152] [I] [ENGINE] hint 池 + vaddr 0x1334b08c 308 KB
[2026-11-05 06:12:31.003] [I] [ENGINE] 安装 @0x7c45db46ac sid=0 mode=4B
[2026-11-05 06:12:33.500] [W] [SLOW] 回调 @0x7c45db46ac 耗时 82 ms
```

---

## 七、常用日志片段

### 7.1 记录安装结果

```c
int rc = zh_hook(fn, cb, user);
if (rc == 0) {
    LOG_I(LOG_HOOK, "✓ 已挂 %s @0x%lx", fn, zh_find(fn));
} else {
    LOG_E(LOG_HOOK, "✗ %s 安装失败 rc=%d (%s)",
          fn, rc, ii_strerror(rc));
    LOG_I(LOG_HOOK, "  建议: %s", ii_strhint(rc));
}
```

### 7.2 统计回调频率

```c
static volatile unsigned long g_calls = 0;

static void on_tick(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_add_fetch(&g_calls, 1, __ATOMIC_RELAXED);
}

// 每秒打一次
static void* stats(void* arg) {
    (void)arg;
    unsigned long last = 0;
    while (1) {
        sleep(1);
        unsigned long cur = __atomic_load_n(&g_calls, __ATOMIC_RELAXED);
        LOG_I(LOG_HOOK, "调用 %lu 次/秒", cur - last);
        last = cur;
    }
    return NULL;
}
```

### 7.3 只在异常时打日志

```c
static void on_check(ii_ctx* c, void* user) {
    (void)user;
    int64_t v = zh_field_get(c, 0, 0x250, 8);
    if (v != 0 && (v < 0x10000 || v > 0x800000000000ULL)) {
        // 只在可疑时打
        LOG_W(LOG_HOOK, "可疑字段值 0x%llx", v);
    }
}
```

### 7.4 首次只打一次

```c
static void on_entry(ii_ctx* c, void* user) {
    (void)user;
    static int once = 0;
    if (__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED) != 0) return;

    LOG_I(LOG_HOOK, "首次调用 this=%p", (void*)c->x[0]);
}
```

---

## 八、注意事项

| 事项 | 说明 |
|---|---|
| 日志有锁 | 但只在内存里，很快 |
| 定期 flush | 500 ms |
| 崩溃最多丢 500 ms | 可接受 |
| 无自动轮转 | 自己清理 |
| 高频函数少打 | 日志文件会暴涨 |
| 用 `__atomic` 计数器 | 代替每帧日志 |

---

## 九、调试技巧

### 9.1 开 TRACE 看每次调用

```c
zlog_set_level(ZLOG_TRACE);

// 然后在回调里
LOG_T(LOG_HOOK, "tick this=%p arg=%ld", (void*)c->x[0], (long)c->x[1]);
```

**警告**：20 TPS 下每秒 20 行，10 分钟 12000 行。

### 9.2 用日志做时序分析

```c
static uint64_t t_start = 0;

static void on_entry(ii_ctx* c, void* user) {
    (void)c; (void)user;
    t_start = zh_now_ms();
}

static void on_ret(ii_ctx* c, void* user) {
    (void)c; (void)user;
    uint64_t dt = zh_now_ms() - t_start;
    LOG_D(LOG_HOOK, "耗时 %llu ms", dt);
}
```

### 9.3 日志对比两次运行

```bash
# 运行 1
adb shell su -c "cat .../zhaohua.log" > run1.log
# 清空
adb shell su -c "> .../zhaohua.log"
# 运行 2
adb shell su -c "cat .../zhaohua.log" > run2.log

diff run1.log run2.log
```

---

## 十、示例：完整日志配置

```c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "zh/zh_layer.h"
#include "zh/zlog.h"

static void rotate_if_big(void) {
    const char* p = "/data/data/com.netease.x19/files/zhprobe/zhaohua.log";
    struct stat st;
    if (stat(p, &st) == 0 && st.st_size > 4 * 1024 * 1024) {
        // 保留旧的一份
        char bak[256];
        snprintf(bak, sizeof bak, "%s.1", p);
        rename(p, bak);
    }
}

static void* worker(void* arg) {
    (void)arg;

    rotate_if_big();

    // 初始化日志（先于引擎）
    zlog_init(NULL, ZLOG_INFO);

    LOG_I(LOG_INIT, "=== 启动 pid=%d ===", getpid());

    int rc = zh_layer_init_default();
    LOG_I(LOG_INIT, "引擎初始化 rc=%d", rc);

    if (rc != ZH_OK && !zh_layer_ready()) {
        LOG_E(LOG_INIT, "初始化失败，退出");
        return NULL;
    }

    char st[128];
    zh_layer_status(st, sizeof st);
    LOG_I(LOG_INIT, "状态: %s", st);

    // 调试期开 DEBUG
    zlog_set_level(ZLOG_DEBUG);

    zh_hook("LocalPlayer_normalTick", on_tick, NULL);

    // 60 秒后降回 INFO
    sleep(60);
    zlog_set_level(ZLOG_INFO);
    LOG_I(LOG_INIT, "降为 INFO 级别");

    while (1) sleep(10);
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
```

---

## 下一步

- [常见陷阱](52-Guide-Pitfalls)
- [错误码](40-Reference-Error-Codes)
- [性能](41-Reference-Performance)
