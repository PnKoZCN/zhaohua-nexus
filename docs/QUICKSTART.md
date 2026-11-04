# 快速上手

5 分钟从零跑通第一个 Hook。

---

## 一、环境要求

| 项 | 要求 |
|---|---|
| 游戏版本 | NetEase Minecraft Bedrock `3.10.5.301327` |
| `libminecraftpe.so` | 323,630,848 字节 |
| 设备架构 | arm64-v8a |
| Root | KernelSU / Magisk（用于注入） |
| 构建工具 | Android NDK r27c |

---

## 二、构建

### 2.1 引擎

```bash
export NDK=/opt/android-ndk-r27c

cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build -j8
```

产物：`build/libzh_engine.so`（约 840 KB）

### 2.2 你的 Mod 模块

```cmake
add_library(my_mod SHARED my_mod.c)
target_link_libraries(my_mod PRIVATE zh_engine)
target_include_directories(my_mod PRIVATE /path/to/zh_engine/include)
```

**关键**：链接时把 `libzh_engine.so` 放在能解析到的地方（与你的 `.so` 同目录）。

---

## 三、写第一个 Hook

```c
#include <pthread.h>
#include <unistd.h>
#include "zh/zh_layer.h"

// 你的回调：每帧被调用
static volatile unsigned long g_ticks = 0;

static void on_tick(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);
    // ★ 这里禁止耗时操作！见第六节
}

// 实际工作线程
static void* worker(void* arg) {
    (void)arg;

    // 1. 初始化引擎
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;

    // 2. 安装 hook（函数名来自数据表）
    int rc = zh_hook("LocalPlayer_normalTick", on_tick, NULL);
    if (rc != 0) return NULL;

    // 3. 你的逻辑（可以是漫长的循环）
    while (1) {
        sleep(5);
        // 读 g_ticks 做你想做的事
    }
    return NULL;
}

// ★ 必须在 constructor 里立刻返回！
__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
```

---

## 四、注入到游戏

```bash
# 1. 找游戏主进程 PID
PID=$(ps -A | grep 'com.netease.x19$' | awk '{print $2}' | head -1)

# 2. 找 libc 和 libdl 的关键函数地址
LIBC=$(grep 'libc.so' /proc/$PID/maps | head -1 | cut -d- -f1)
LIBDL=$(grep 'libdl.so' /proc/$PID/maps | head -1 | cut -d- -f1)
# mmap = libc + 0xF0000（按实际版本查）
# dlopen = libdl + 0x101c

# 3. 把 so 拷到游戏 lib 目录
LIBDIR=/data/app/*/com.netease.x19*/lib/arm64
cp libzh_engine.so $LIBDIR/
cp libmy_mod.so $LIBDIR/
chmod 755 $LIBDIR/libzh_engine.so $LIBDIR/libmy_mod.so

# 4. 注入引擎（只一次）
./zhinf $PID libzh_engine.so <mmap_addr> <dlopen_addr>

# 5. 注入你的模块
./zhinf $PID libmy_mod.so <mmap_addr> <dlopen_addr>

# 6. 验证后立即删除注入的 so（保持磁盘干净）
rm $LIBDIR/libmy_mod.so
```

---

## 五、常用 API 速查

### 5.1 函数 Hook

```c
// 入口 hook
zh_hook("Actor_getPos", cb, user);

// 带优先级（多个回调时数字大的先跑）
zh_hook_ex("Actor_getPos", cb, user, 300);

// 返回点 hook（函数 ret 前）
zh_hook_ret("Actor_getPos", cb, user);

// 卸载
zh_unhook("Actor_getPos");
```

### 5.2 读参数与返回值

```c
static void cb(ii_ctx* c, void* user) {
    // 读参数（x0 = this, x1 = arg1, ...）
    void* p_this = (void*)c->x[0];
    int   arg1   = (int)c->x[1];

    // 读返回值（在返回点 hook 里）
    int ret = (int)zh_get_ret_int(c);
    float f = zh_get_ret_float(c);

    // 改返回值
    zh_set_ret_int(c, 42);
    zh_set_ret_float(c, 3.14f);
}
```

### 5.3 字段读写

```c
// 读 this 的字段
int64_t hp = zh_field_get(c, 0, 0x250, 8);      // base=x0, off=0x250, size=8

// 写
zh_field_set(c, 0, 0x250, 8, 999);

// 便捷宏
#define ZH_THIS_I32(c, off) ((int32_t)zh_field_get((c), 0, (off), 4))
#define ZH_THIS_PTR(c, off) ((void*)(uintptr_t)zh_field_get((c), 0, (off), 8))
```

### 5.4 栈局部变量

```c
// 枚举函数的栈槽
int n = ii_local_count("ItemStack_isNull");
for (int i = 0; i < n; i++) {
    uint16_t spOff; uint8_t size; uint32_t insnOff;
    ii_local_slot("ItemStack_isNull", i, &spOff, &size, &insnOff);
}

// 在 hook 里读
int64_t v = zh_local_get(c, 0x8, 4);    // sp+8, 4 字节
```

### 5.5 常量修改

```c
// 查函数内某常量出现次数
int cnt = zh_const_count("LocalPlayer_normalTick", 16);

// 在该常量指令处插桩
zh_modify_const("LocalPlayer_normalTick", 16, 0, cb, user);

// 在回调里改寄存器实现常量替换
static void cb(ii_ctx* c, void* user) {
    // 原指令是 cmp w0, #16 → 把 16 改成 100
    // 具体改哪个寄存器取决于那条指令的编码
}
```

### 5.6 调用重定向

```c
static void* on_call(ii_ctx* c, void* user) {
    // 返回 NULL = 走原目标
    // 返回函数地址 = 跳到你的实现
    return (void*)my_replacement;
}

zh_redirect("ContainerScreenController_getStackFromModel",
            "ItemStack_isNull", on_call, NULL);
```

### 5.7 崩溃隔离

```c
// 用 guarded 版安装：回调崩溃不会带崩游戏
zh_hook_guarded("SomeUnstableFunc", risky_cb, NULL);

// 查已隔离次数
long n = zh_guard_caught();
```

### 5.8 日志

```c
#include "zh/zlog.h"

zlog_init("/data/data/com.netease.x19/cache/my_mod.log");
zlog_set_level(ZLOG_INFO);

LOG_I(LOG_HOOK, "hook 成功 0x%lx", addr);
LOG_W(LOG_POOL, "池已满");
LOG_E(LOG_HOOK, "安装失败 rc=%d", rc);
```

### 5.9 多实例（隔离不同模块）

```c
ii_engine* e = ii_engine_create();
ii_engine_install(e, addr, cb, user);
// ...
ii_engine_destroy(e);      // 只撤自己那份回调
```

---

## 六、⚠️ 最重要的约束

### 6.1 回调里禁止耗时操作

**回调运行在游戏线程上，任何阻塞都会冻结游戏。**

```c
// ❌ 绝对禁止
static void bad_cb(ii_ctx* c, void* u) {
    save_to_disk();       // 文件 I/O
    http_request();       // 网络
    sleep(1);             // 直接 ANR
    pthread_mutex_lock(&busy_lock);   // 可能长时间等待
}

// ✅ 正确
static void good_cb(ii_ctx* c, void* u) {
    queue_push(&g_q, snapshot(c));   // 只入队
}

// 独立线程消费
static void* consumer(void* arg) {
    while (1) {
        item* it = queue_pop(&g_q);
        if (it) do_heavy_work(it);
    }
}
```

**引擎会在回调超过 50 ms 时告警**：
```
[SLOW] 回调 @0x7c4cfba6c8 耗时 520 ms（阈值 50）。
回调运行在游戏线程上，重操作会冻结游戏，请改为异步。
```

**Android ANR 阈值是 5 秒。** 超过即闪退。

### 6.2 `constructor` 里禁止 `sleep`

```c
// ❌ 会阻塞 dlopen → 游戏卡死 → ANR
__attribute__((constructor))
static void run(void) {
    zh_hook(...);
    sleep(30);
}

// ✅
__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
```

**这是最容易犯的错误** —— 20 轮调试的崩溃全部源于此。

### 6.3 一次只注入一个库

```
注入 → 验证 → 立即删除 → 再注入下一个
```

**多个库同时驻留会互相干扰，且难以定位问题。**

---

## 七、常用函数名（数据表内）

### 7.1 高频调用（适合做 tick 类 hook）

| 函数 | 频率 | 用途 |
|---|---|---|
| `LocalPlayer_normalTick` | 20 TPS | 玩家 tick |
| `Level_tick` | 20 TPS | 世界 tick |
| `Actor_getPos` | 高 | 读实体坐标 |
| `Actor_getLevel` | 高 | 读所在世界 |

### 7.2 常用字段（`Actor` 类）

| 字段 | 偏移 | 类型 |
|---|---|---|
| `Actor` 的 `Level*` | `+0x250` | 指针 |
| 位置 | 见 `fields_real.h` | Vec3 |
| 血量 | 见 `fields_real.h` | float |

**完整列表见 `data/fields_real.h`（716 条）与 `data/ret_db.h`（124 个函数）**。

### 7.3 查询方法

```c
// 函数是否存在
unsigned long addr = zh_find("Actor_getPos");
if (!addr) { /* 不在数据表 */ }

// 查函数内有哪些常量
for (int i = 0; i < zh_tbl_count(ZH_TBL_CONST); i++) { ... }

// 查字段访问点
for (int i = 0; i < zh_tbl_count(ZH_TBL_FIELDACC); i++) {
    const char* fn; uint16_t off; int base, size, wr;
    zh_tbl_fieldacc(i, &fn, &off, &base, &size, &wr);
    if (strcmp(fn, "Actor_getPos") == 0) {
        // 找到了
    }
}
```

---

## 八、调试技巧

### 8.1 确认 hook 是否生效

```c
static volatile unsigned long g_hits = 0;
static void cb(ii_ctx* c, void* u) { __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED); }

// 5 秒后检查
sleep(5);
// g_hits 应接近 100（20 TPS × 5 秒）
```

**0 次说明**：
- 函数未被调用（换一个更频繁的）
- 数据表偏移错误（确认版本）
- hook 安装失败（看 `rc`）

### 8.2 确认指令已改写

```c
// 读入口原始 4 字节
uint32_t w = *(volatile uint32_t*)addr;
int is_branch = ((w & 0xFC000000) == 0x14000000);   // b 指令
```

### 8.3 看引擎日志

```bash
cat /data/data/com.netease.x19/files/zhprobe/watchdog.log
```

**关键标记**：
```
[destroy]          实例销毁
[alloc_shell]      内存池分配
[dispatch]         调度
[SLOW]             回调太慢 ⚠️
[retire]           延迟释放队列
POOL DEAD          内存池失效
```

### 8.4 崩溃排查顺序

1. **有没有 `[SLOW]` 告警？** → 回调里有阻塞
2. **ANR 日志**（`/data/system/dropbox/`）→ `Input dispatching timed out` 说明主线程被卡
3. **回调是否用了 `sleep` / 文件 I/O？** → 改异步
4. **`zh_unhook` 后是否还崩溃？** → 卸载残留
5. **换一个函数 hook 是否也崩？** → 数据表问题

---

## 九、完整示例

```c
// my_mod.c —— 完整可用的最小 Mod
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "zh/zh_layer.h"
#include "zh/zlog.h"

#define LOG_PATH "/data/data/com.netease.x19/cache/my_mod.log"

static volatile unsigned long g_ticks = 0;
static volatile int g_hp_fixed = 0;

// tick 回调：只做计数
static void on_tick(ii_ctx* c, void* u) {
    (void)c; (void)u;
    __atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);
}

// 血量回调：改字段
static void on_get_health(ii_ctx* c, void* u) {
    (void)u;
    // 从返回点读返回值，若低于阈值则改成满血
    float hp = zh_get_ret_float(c);
    if (hp > 0.0f && hp < 10.0f) {
        zh_set_ret_float(c, 20.0f);
        __atomic_add_fetch(&g_hp_fixed, 1, __ATOMIC_RELAXED);
    }
}

static void* worker(void* arg) {
    (void)arg;
    zlog_init(LOG_PATH);
    zlog_set_level(ZLOG_INFO);

    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) {
        LOG_E(LOG_HOOK, "引擎初始化失败");
        return NULL;
    }
    LOG_I(LOG_HOOK, "引擎就绪 base=0x%lx", zh_module_base());

    int r1 = zh_hook("LocalPlayer_normalTick", on_tick, NULL);
    LOG_I(LOG_HOOK, "tick hook rc=%d", r1);

    // Actor_getHealth 有返回点的话用 zh_hook_ret
    int r2 = zh_hook_ret("Actor_getHealth", on_get_health, NULL);
    LOG_I(LOG_HOOK, "health hook rc=%d", r2);

    while (1) {
        sleep(10);
        LOG_I(LOG_HOOK, "ticks=%lu hp_fixed=%d guard=%ld",
              __atomic_load_n(&g_ticks, __ATOMIC_RELAXED),
              __atomic_load_n(&g_hp_fixed, __ATOMIC_RELAXED),
              zh_guard_caught());
    }
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
```

---

## 十、目录参考

```
zh_engine/
├── include/zh/          公共头文件（8 个）
│   ├── zh_layer.h       ★ 门面层，先看这个
│   ├── instr_hook.h     底层 API
│   ├── zh_tbl.h         数据表查询
│   ├── zh_db.h          运行期数据表
│   ├── zlog.h           日志
│   └── ...
├── data/                数据表（偏移都在这里）
│   ├── fields_real.h    字段偏移（716 条）
│   ├── ret_db.h         返回点（2,754 条）
│   ├── const_db.h       常量（26,447 条）
│   └── ...
├── examples/steps/      真机测试示例（27 个）
│   ├── t03_hook.c       最简单的 hook
│   ├── t08d_async.c     ★ 正确的长跑写法
│   └── ...
└── tests/               主机单元测试（12 个）
```

**建议阅读顺序**：
1. `examples/steps/t03_hook.c` —— 最小 hook
2. `examples/steps/t08d_async.c` —— 正确写法
3. `include/zh/zh_layer.h` —— 门面 API
4. `data/fields_real.h` —— 有哪些字段可用
