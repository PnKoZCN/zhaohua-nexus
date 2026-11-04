# 第一个 Hook：5 分钟跑通

本页带你从零写出第一个能跑的 Mod。

---

## 一、准备

| 项 | 要求 |
|---|---|
| 游戏 | NetEase MC Bedrock `3.10.5.301327` |
| 设备 | arm64-v8a，已 root（KernelSU / Magisk） |
| 构建 | Android NDK r27c |
| 引擎 | `libzh_engine.so`（从源码构建或预编译） |

---

## 二、完整代码

新建 `my_mod.c`：

```c
// my_mod.c —— 最小可用 Mod：统计某函数被调用多少次
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "zh/zh_layer.h"

#define LOG_PATH "/data/data/com.netease.x19/cache/my_mod.log"

static int g_logfd = -1;

// 简易日志：回调里不能用 zlog（会做文件 I/O），这里只入内存
static volatile unsigned long g_hits = 0;

// ═══════════════════════════════════════════════════════════
// 回调：每帧会被调用
//
// ★ 铁律：只做轻量操作。禁止文件 I/O、sleep、网络、锁等待。
// ═══════════════════════════════════════════════════════════
static void on_tick(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);
}

// ═══════════════════════════════════════════════════════════
// 工作线程：所有重活都在这里
// ═══════════════════════════════════════════════════════════
static void* worker(void* arg) {
    (void)arg;

    // 1. 初始化引擎
    int rc = zh_layer_init_default();
    if (rc != ZH_OK && !zh_layer_ready()) {
        return NULL;
    }

    // 2. 确认目标函数存在
    unsigned long addr = zh_find("LocalPlayer_normalTick");
    if (!addr) {
        return NULL;   // 函数不在数据表里
    }

    // 3. 安装 hook
    rc = zh_hook("LocalPlayer_normalTick", on_tick, NULL);
    if (rc != 0) {
        return NULL;
    }

    // 4. 主循环：定期输出统计
    while (1) {
        sleep(5);
        unsigned long h = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
        // 20 TPS × 5 秒 = 约 100 次
        // 可以用你自己的方式输出（这里是写文件）
        char buf[64];
        int n = snprintf(buf, sizeof buf, "hits=%lu\n", h);
        // ... 写文件
        (void)n;
    }

    return NULL;
}

// ═══════════════════════════════════════════════════════════
// ★ 关键：constructor 必须立刻返回
//
// 如果在这里 sleep，会阻塞 dlopen 的调用线程。
// 若那正好是游戏主循环 / 渲染线程 → 触摸 5 秒无人处理 → ANR → 闪退。
// ═══════════════════════════════════════════════════════════
__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) {
        pthread_detach(t);
    }
    // 立刻返回，不阻塞
}
```

---

## 三、构建

### 3.1 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_mod C)

add_library(my_mod SHARED my_mod.c)

# 链接引擎
target_link_libraries(my_mod PRIVATE zh_engine)

# 头文件路径（改成你的实际路径）
target_include_directories(my_mod PRIVATE
    ${CMAKE_SOURCE_DIR}/../zh_engine/include)
```

### 3.2 构建命令

```bash
export NDK=/opt/android-ndk-r27c
export ZH=/path/to/zh_engine

cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release \
  -Dzh_engine_DIR=$ZH/build

cmake --build build -j8
```

产物：`build/libmy_mod.so`

### 3.3 校验产物

```bash
readelf -dW build/libmy_mod.so | grep NEEDED
# 应该看到 libzh_engine.so

readelf -lW build/libmy_mod.so | grep LOAD | head -1
# 页对齐应该是 0x4000（Android 15+ 要求）
```

---

## 四、部署与注入

### 4.1 拷贝到游戏 lib 目录

```bash
LIBDIR=$(ls -d /data/app/*/com.netease.x19*/lib/arm64 | head -1)

cp libzh_engine.so   $LIBDIR/
cp libmy_mod.so      $LIBDIR/
chmod 755 $LIBDIR/libzh_engine.so $LIBDIR/libmy_mod.so
```

### 4.2 找游戏 PID 与关键地址

```bash
PID=$(ps -A | grep 'com.netease.x19$' | awk '{print $2}' | head -1)

# mmap 和 dlopen 的地址（每次启动会变，需重新查）
# 方法见《构建与注入》一页
```

### 4.3 注入

```bash
# 1. 先注入引擎（只一次）
./zhinf $PID libzh_engine.so <mmap_addr> <dlopen_addr>

# 2. 再注入你的模块
./zhinf $PID libmy_mod.so <mmap_addr> <dlopen_addr>

# 3. 验证后立即删除（保持磁盘干净）
rm $LIBDIR/libmy_mod.so
```

### 4.4 验证

```bash
# 看引擎日志
adb shell cat /data/data/com.netease.x19/files/zhprobe/watchdog.log

# 看你的输出
adb shell cat /data/data/com.netease.x19/cache/my_mod.log
```

**期望**：每 5 秒输出一次 `hits=...`，数值每次增加约 100。

---

## 五、常见问题

### Q1: `hits` 一直是 0

**原因**（按概率排序）：

| 原因 | 排查 |
|---|---|
| 函数没被调用 | 换一个更高频的函数（如 `Actor_getPos`） |
| 数据表版本不匹配 | `zh_find()` 返回 0 会提前退出，看第 2 步 |
| hook 安装失败 | 检查 `zh_hook()` 的返回值 |
| 注入到了错误的进程 | `ps` 确认 PID 是主进程（线程数 > 50） |

**诊断代码**：

```c
unsigned long addr = zh_find("LocalPlayer_normalTick");
if (!addr) {
    LOG_E(LOG_HOOK, "函数不在数据表！版本不匹配？");
    return NULL;
}
int rc = zh_hook("LocalPlayer_normalTick", on_tick, NULL);
LOG_I(LOG_HOOK, "hook rc=%d addr=0x%lx", rc, addr);
```

### Q2: 注入后游戏闪退

**99% 是 `constructor` 里阻塞了。**

```c
// ❌ 错误
__attribute__((constructor))
static void run(void) {
    zh_layer_init_default();   // 这本身没问题
    zh_hook(...);
    sleep(10);                 // ← 这一行就要命
}

// ✅ 正确
__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
```

**验证**：看 ANR 日志
```bash
adb shell cat /data/system/dropbox/ | grep -A2 "Subject:"
# 如果看到 "Input dispatching timed out" 就是这个原因
```

### Q3: 引擎日志里出现 `[SLOW]`

```
[SLOW] 回调 @0x7c4cfba6c8 耗时 520 ms（阈值 50）。
```

**回调里有阻塞操作。** 改成只入队，异步处理。

### Q4: `zh_hook` 返回负数

| 返回码 | 含义 | 处理 |
|---|---|---|
| `-1` | 参数无效 | 检查 `addr` 非 0 |
| `-2` | 已存在站点 | 改用 `ii_add_callback` 追加 |
| `-3` | 站点表满 | 卸载不用的 |
| `-5` | 指令不可插桩 | 换函数 |
| `-10` | 写指令失败 | `mprotect` 失败，检查 SELinux |
| `-13` | 后续指令不干净 | 罕见，换函数 |
| `-15` | shell 分配失败 | 内存池耗尽 |

完整列表见 [错误码](40-Reference-Error-Codes)。

---

## 六、进阶：改返回值

统计只是观察。要**改变游戏行为**，用返回点 hook：

```c
// 让 Actor_getHealth 永远返回 20
static void on_health(ii_ctx* c, void* user) {
    (void)user;
    zh_set_ret_float(c, 20.0f);
}

// 在 worker 里
zh_hook_ret("Actor_getHealth", on_health, NULL);
```

**注意**：
- 返回点 hook 需要数据表里有该函数的 `retpoint` 记录
- 类型必须匹配：`float` 函数用 `zh_set_ret_float`
- 先用 `ii_ret_kind(addr)` 确认类型

---

## 七、进阶：改字段

```c
// 在任意 hook 里读写 this 的字段
static void on_tick(ii_ctx* c, void* user) {
    (void)user;

    // 读 this->某字段（偏移从数据表查，假设是 0x250，8 字节指针）
    int64_t v = zh_field_get(c, 0, 0x250, 8);

    // 写回去（比如翻倍）
    zh_field_set(c, 0, 0x250, 8, v);
}
```

**查偏移**：
```c
for (int i = 0; i < zh_tbl_count(ZH_TBL_FIELDACC); i++) {
    const char* fn; uint16_t off; int base, size, wr;
    zh_tbl_fieldacc(i, &fn, &off, &base, &size, &wr);
    if (strcmp(fn, "Actor_getLevel") == 0) {
        // off=0x250, base=0(x0), size=8
    }
}
```

---

## 八、检查清单

上线前逐项确认：

- [ ] `constructor` 里**没有** `sleep` / 文件 I/O / 网络
- [ ] 所有回调里**只有**内存操作和原子加
- [ ] 重活都在 `worker` 线程
- [ ] `zh_find()` 返回值检查过
- [ ] `zh_hook()` 返回码检查过
- [ ] 验证完**删除**了注入的 `.so`
- [ ] 看日志没有 `[SLOW]`
- [ ] 游戏存活超过 30 秒

---

## 九、下一步

- [构建与注入详解](04-Getting-Started-Build-Inject)
- [站点与回调链原理](10-Core-Site-And-Chain)
- [常见陷阱](52-Guide-Pitfalls) ★ 强烈建议
