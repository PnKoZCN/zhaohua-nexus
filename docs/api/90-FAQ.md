# FAQ

---

## 一、入门

### Q：这个框架是什么？

**ZhaoHua Nexus** 是网易 Minecraft 基岩版（`3.10.5.301327`）的原生 ARM64 hook 框架。用 C 写 Mod，`dlopen` 注入，不需要 root。

### Q：和 Fabric Mixin 什么关系？

**思路对标，实现完全不同**：
- Mixin 改字节码（Java），Nexus 改指令（ARM64）
- Mixin 由 JVM 加载器执行，Nexus 直接注入进程
- Mixin 靠反射拿字段名，Nexus 靠数据表拿偏移

对比见 [vs Fabric Mixin](02-Introduction-vs-Fabric-Mixin)。

### Q：为什么要用这个而不用别的方法？

| 方法 | 问题 |
|---|---|
| 改 APK | 破坏签名，无法联机 |
| Frida | 需要 root，性能差 |
| 内存修改器 | 只能改数据，不能改逻辑 |
| **Nexus** | 原生代码，root-free，性能接近零开销 |

### Q：需要 root 吗？

**引擎本身不需要。** 但**第一次注入**需要一个能把 `.so` 塞进游戏进程的通道（如 KernelSU 的 `zhinf`）。

**注入后，框架运行完全在游戏进程内，不依赖 root。**

---

## 二、构建

### Q：为什么必须在 NDK 下构建？

**musl gcc 产出的 `.so` 无法被 Android 的 bionic `dlopen` 加载**（libc ABI 不兼容）。

**必须用 NDK**（本机用 `r27c`）。

```bash
export ANDROID_NDK=/opt/android-ndk-r27c
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
      -DZH_PORTABLE=OFF
cmake --build build -j8
```

### Q：`ZH_PORTABLE=ON` 是什么？

**去掉 10 个 bionic 特有的符号**，让 `.so` 能在 Alpine/musl 下加载。

**用途**：QEMU 里验证逻辑。

**生产必须 `OFF`。**

### Q：构建有多少警告？

**零警告。** 用 `-Wall -Wextra -Werror`。

### Q：能做 x86_64 构建吗？

**可以，但 hook 核心是 stub。**

`ZH_ARCH_ARM64` 未定义时，`ii_common` / `ii_light` / `ii_redirect_common` / `ii_cancel_tramp` 都是空实现。

**用途**：跑 host 单元测试（12/12 通过）。

---

## 三、使用

### Q：最小可用例子？

```c
#include <pthread.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static void on_tick(ii_ctx* c, void* user) {
    (void)c; (void)user;
    // 你的逻辑
}

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;
    zh_hook("LocalPlayer_normalTick", on_tick, NULL);
    while (1) sleep(5);
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}

__attribute__((destructor))
static void cleanup(void) { zh_unhook_all(); }
```

**完整版见 [第一个 Hook](03-Getting-Started-First-Hook)。**

### Q：为什么 `constructor` 里要开线程？

**因为 `constructor` 在 `dlopen` 内部同步执行。在里面阻塞会冻结注入线程。**

**这是本项目最大的坑。** 详见 [陷阱 1](52-Guide-Pitfalls)。

### Q：能 hook 任意函数吗？

**不能。** 只有数据表里的 124 个函数能被 `zh_find` 找到。

**但你可以在知道地址的情况下用 `ii_install(addr, ...)`。** 代价是版本更新后要重算。

### Q：怎么知道有哪些函数可用？

```c
for (int i = 0; i < zh_tbl_count(ZH_TBL_RETFUNC); i++) {
    LOG_I(LOG_HOOK, "%s", zh_tbl_retfunc(i));
}
```

### Q：一个函数能挂几个回调？

**不限**（受内存限制）。

```c
zh_hook(fn, cb1, NULL);
ii_add_callback(addr, cb2, NULL, 100);
ii_add_callback(addr, cb3, NULL, 200);
```

**执行顺序按 priority 降序。**

### Q：怎么同时拿到参数和返回值？

**入口 hook 存参数，返回点 hook 用返回值：**

```c
static __thread void* t_this = NULL;

static void on_entry(ii_ctx* c, void* user) { t_this = (void*)c->x[0]; }
static void on_ret(ii_ctx* c, void* user)   { /* 用 t_this 和 zh_get_ret_* */ }
```

### Q：改返回值为什么没生效？

**检查三件事**：

1. 是不是在**返回点** hook（入口没有返回值）
2. 类型对不对（`float` 用 `zh_set_ret_float`，不是 `zh_set_ret_i32`）
3. 多返回点函数是否**全挂**了（`ii_install_at_return(fn, -1, ...)`）

---

## 四、稳定性

### Q：为什么注入后游戏不崩，但几秒后闪退？

**大概率是三个原因之一**：

| 原因 | 检查 |
|---|---|
| `constructor` 里阻塞 | `constructor` 里有 `sleep`/`while(1)` 吗 |
| 往游戏内存写了东西 | `zhreg_add_pool` 的 `kind` 对了吗 |
| 回调太慢 | 日志里有 `[SLOW]` 吗 |

**先看 logcat 有没有 ANR。**

### Q：为什么卸载后崩？

**`destructor` 里没 `zh_unhook_all()`。**

**站点还在，但 shell 已在野内存。**

### Q：崩溃隔离为什么没救回来？

**检查**：

1. `zh_guard_on()` 了吗（默认开）
2. 崩的是回调内部，还是回调改了 ctx 导致崩在游戏里（**后者救不了**）
3. 是不是无限循环（**隔离不管这个**）

### Q：怎么定位崩溃点？

```bash
adb logcat | grep -E "DEBUG|tombstone|SIGSEGV"
adb shell su -c "cat /data/tombstones/tombstone_00"
```

**如果崩溃在 `ii_common` 或 `libzh_engine.so` → 框架问题。**

**如果崩溃在 `libminecraftpe.so` → 你的回调改坏了东西。**

### Q：能同时注入多个 Mod 库吗？

**能，但要极其小心：**

1. 每个库都要 `zh_set_owner("唯一名字")`
2. 每个库都要 `destructor` 清理
3. 共享区（`poolreg.bin`）会自动协调

**框架本身就支持多库共存**：单站点最多挂 32 个库的回调（`ZHREG_MAX_CB`），
全进程共享 8192 个站点（`ZHREG_MAX_SITE`），单库上限 1024。

建立者先卸载而别人还在用时，入口会保留、shell 不归还，建立者身份转交 ——
不会把别人的 hook 一起拆掉。

**开发调试期建议一次只装一个**，那是为了精确定位问题，不是框架限制。

---

## 五、性能

### Q：hook 会影响游戏帧率吗？

**实测 t08d：30 秒 601 次回调，游戏稳定 20 TPS，帧率无变化。**

**单次 FULL 回调 280 ns，LIGHT 120 ns。**

### Q：多少个 hook 才会有明显影响？

**按 20 TPS 算**：

```
280 ns × 20 = 5.6 μs/秒 = 0.0006% CPU
```

**100 个 hook 也才 0.06%。**

**真正的性能杀手是你在回调里做的事。**

### Q：什么时候该用 LIGHT？

| 场景 | 选择 |
|---|---|
| 20+ TPS 的 tick | LIGHT |
| 万次/秒的热函数 | LIGHT |
| 需要改返回值 | FULL（必须） |
| 需要 cancel | FULL（必须） |
| 不常用函数 | FULL |

### Q：隔离开销大吗？

**+25 ns（280 → 305），约 9%。**

**高频函数可以关：**

```c
zh_guard_off();
```

---

## 六、数据表

### Q：数据表从哪来？

**从 `libminecraftpe.so` 静态扫描得来。**

**流程见 [偏移采集](50-Guide-Offset-Collection)。**

### Q：游戏更新后还能用吗？

**不能。所有偏移都变。**

**必须重采。见 [版本更新](51-Guide-Version-Update)。

### Q：`zh_find` 返回 0 怎么排查？

```c
// 1. 引擎初始化了吗
if (!zh_layer_ready()) { /* 先初始化 */ }

// 2. 表加载了吗
LOG_I(LOG_TBL, "表 %d 条", zh_tbl_total());   // 应该是 31407

// 3. 名字对吗
for (int i = 0; i < zh_tbl_count(ZH_TBL_RETFUNC); i++) { ... }

// 4. 版本对吗
LOG_I(LOG_TBL, "版本 0x%llx", zh_game_version());
```

### Q：能自己加函数吗？

**加进 `data/ret_db.h`，重新 `pack_db.py`。**

**但你需要自己找偏移。**

---

## 七、定位符

### Q：`zh_find` 和 `ii_install` 有什么区别？

| | `zh_find` | `ii_install` |
|---|---|---|
| 输入 | 函数名 | 绝对地址 |
| 查表 | ✅ | ❌ |
| 版本适应 | ✅ | ❌ |
| 用于 | 常规场景 | 表里没有的函数 |

### Q：`ii_install_at_return` 的 `nth` 怎么用？

```c
ii_install_at_return(fn, 0, cb, NULL);    // 第一个返回点
ii_install_at_return(fn, 3, cb, NULL);    // 第四个
ii_install_at_return(fn, -1, cb, NULL);   // 全部
```

### Q：怎么查函数有几个返回点？

```c
int n = ii_retpoint_count(fn);
```

**实测 `ItemStack_isNull` 有 28 个。**

---

## 八、贡献 / 二次开发

### Q：源码结构？

```
include/zh/     公共头文件
src/            实现
data/           数据表（9 个 .h + 1 个 .bin）
tests/          host 测试（12 个）
examples/steps/ 真机测试（27 个）
tools/          打包 / 校验脚本
```

**总计 7,229 行（5,802 C + 1,427 头文件）。**

### Q：怎么跑 host 测试？

```bash
cmake -B build-host -DZH_BUILD_TESTS=ON
cmake --build build-host -j8
cd build-host && LD_LIBRARY_PATH=. ./tests/test_db
```

**12 个测试应全过。**

### Q：怎么加新的数据表？

1. 在 `pack_db.py` 的 `TABLES` 里加一行
2. 加对应的 C 结构体 + 静态断言
3. 加 `zh_tbl_xxx()` 访问函数
4. 重打包

---

## 九、其它

### Q：`zh` / `ii` / `zlog` 前缀什么意思？

| 前缀 | 含义 |
|---|---|
| `zh` | ZhaoHua（门面层，用户 API） |
| `ii` | InstrHook（指令层，底层） |
| `zlog` | ZhaoHua Log |

**`zh_` 是给用户用的，`ii_` 是底层。**

### Q：为什么叫 Nexus？

**"Nexus" = 连接点**。

**它把游戏内部的逻辑和你的 Mod 代码连起来。**

### Q：许可证？

**CC BY-NC-SA 4.0**（署名-非商业性使用-相同方式共享 4.0 国际），
另加原作者的附加条款。不是 MIT。

| 要求 | 对应条款 |
|---|---|
| 禁止商用 | 第 2(a)(1) 条 NonCommercial |
| 必须开源 | 第 3(b) 条 ShareAlike |
| 必须署名 | 第 3(a) 条 Attribution |

**为什么不用 MIT**：MIT 允许商业使用与闭源，会让本框架被直接打包进商业
外挂销售，作者无法阻止。CC BY-NC-SA 4.0 是同时满足"禁商用 + 强制开源 +
强制署名"三个要求的成熟协议。

**附加条款**（见仓库 README）：

1. **署名格式** —— 分发时必须显著注明
   ```
   本项目基于 ZhaoHua Nexus 开发
   https://github.com/PnKoZCN/zhaohua-nexus
   ```
2. **严禁开发游戏外挂** —— 禁止任何联网多人环境中的作弊功能、
   绕过反作弊、商业外挂、损害其他玩家利益
3. **免责声明** —— 可能封号/数据丢失，风险自负

**允许的用途**：纯单机/离线、获许可的私人测试服、单人自身账号、安全研究。

**违反任一条款，授权立即终止。**

### Q：怎么报告问题？

**GitHub**: https://github.com/PnKoZCN/zhaohua-nexus/issues

**附带**：
- 游戏版本（`.so` 大小 + sha256）
- 错误码
- 相关日志片段

---

## 相关问题

- [错误码详解](40-Reference-Error-Codes)
- [常见陷阱](52-Guide-Pitfalls)
- [性能](41-Reference-Performance)
