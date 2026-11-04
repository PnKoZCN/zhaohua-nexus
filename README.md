# ZhaoHua Nexus

**Native hook framework for NetEase Minecraft Bedrock — Fabric Mixin-like developer experience**

> 面向 NetEase Minecraft Bedrock 的原生 Hook 框架，对标 Fabric Mixin 的开发体验。

---

## 特性

| 能力 | 对应 Mixin | 状态 |
|---|---|---|
| 函数入口 Hook | `@Inject` | ✅ |
| 返回点 Hook | `@Inject(at = RETURN)` | ✅ |
| 多回调链 | 多个 `@Inject` | ✅ 按 priority 排序 |
| 字段读写 | `@Accessor` | ✅ |
| 常量修改 | `@ModifyConstant` | ✅ |
| 调用重定向 | `@Redirect` | ✅ |
| 栈局部变量读取 | `@Local` | ✅ |
| 崩溃隔离 | — | ✅ 回调崩溃不带崩游戏 |
| 运行期数据表 | — | ✅ 换数据不用重编译 |
| 多实例 | — | ✅ 按归属精确卸载 |

**已在真机验证 15 项能力，30 秒 / 601 次回调长跑稳定。**

---

## 快速开始

```c
#include <pthread.h>
#include "zh/zh_layer.h"

static volatile unsigned long g_ticks = 0;

// ★ 回调运行在游戏线程上，只能做轻量操作
static void on_tick(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);
}

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;
    zh_hook("LocalPlayer_normalTick", on_tick, NULL);
    while (1) sleep(5);          // 你的逻辑
    return NULL;
}

// ★ constructor 必须立刻返回，否则阻塞 dlopen → 游戏 ANR
__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
```

### 构建

```bash
export NDK=/opt/android-ndk-r27c

cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build -j8
```

产物：`build/libzh_engine.so`

### 主机侧测试（无需设备）

```bash
cmake -B build-host -DCMAKE_BUILD_TYPE=Release -DZH_BUILD_TESTS=ON
cmake --build build-host -j8
cd build-host && LD_LIBRARY_PATH=. ./tests/test_insn   # 共 12 个
```

---

## ⚠️ 三条铁律

### 1. 回调里禁止耗时操作

回调运行在**游戏线程**上，任何阻塞都会冻结游戏。Android ANR 阈值 **5 秒**。

```c
// ❌ 禁止
static void cb(ii_ctx* c, void* u) {
    save_to_disk();   // 文件 I/O
    sleep(1);         // 直接 ANR
}

// ✅ 正确：只入队，异步处理
static void cb(ii_ctx* c, void* u) {
    queue_push(&g_q, snapshot(c));
}
```

引擎在超过 50 ms 时告警：`[SLOW] 回调 @0x... 耗时 N ms`

### 2. `constructor` 里禁止 `sleep`

```c
// ❌ 阻塞 dlopen → 游戏卡死 → ANR
__attribute__((constructor))
static void run(void) { sleep(30); }

// ✅
__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
```

### 3. 一次只注入一个库

注入 → 验证 → 立即删除 → 再注入下一个。

---

## 版本要求

| 项 | 要求 |
|---|---|
| 游戏 | NetEase MC Bedrock **3.10.5.301327** |
| `libminecraftpe.so` | 323,630,848 字节 |
| 架构 | arm64-v8a |
| Root | KernelSU / Magisk（注入用） |
| 构建 | Android NDK r27c |

**换版本必须重新采集偏移**，见 [docs/OFFSET_GUIDE.md](docs/OFFSET_GUIDE.md)。

---

## 数据表

| 表 | 条目数 | 用途 |
|---|---|---|
| `rettype` | 124 | 函数返回类型 |
| `blcall` | 449 | 方法调用点 |
| `const` | 26,447 | 函数内常量 |
| `frame` | 120 | 栈帧信息 |
| `slot` | 547 | 栈槽位 |
| `fieldacc` | 716 | 字段访问点 |
| `constfunc` | 121 | 含常量的函数 |
| `poolhint` | 5 | 内存池提示 |
| `retfunc` | 124 | 含返回点的函数 |
| `retpoint` | 2,754 | 返回点地址 |
| **合计** | **31,407** | 367,193 字节 |

支持**双数据源**：编译期静态表（`data/*.h`）或运行期 mmap（`zh_db_full.bin`）：

```c
zh_db db;
if (zh_db_load(&db, "/path/zh_db_full.bin") == ZH_DB_OK)
    zh_tbl_attach(&db);      // 之后所有查询走 mmap 数据
zh_tbl_attach(NULL);         // 退回静态表
```

---

## 项目结构

```
include/zh/          公共头文件
  zh_layer.h         ★ 门面层，先看这个
  instr_hook.h       底层 API（189 个导出）
  zh_tbl.h           数据表查询
  zh_db.h            运行期数据表
  zlog.h             日志
src/                 实现（5,802 行）
data/                数据表（偏移都在这里）
tests/               12 个主机单元测试
examples/steps/      27 个真机测试
  t03_hook.c         最小 hook
  t08d_async.c       ★ 正确的长跑写法
tools/               打包 + 校验工具
docs/                设计文档 + API 手册
```

---

## 已验证的测试

| 测试 | 能力 | 结果 |
|---|---|---|
| t01 | 引擎初始化 | ✅ |
| t02 | 函数查找 | ✅ 124/124 |
| t03 | 装/卸 hook | ✅ 指令完整恢复 |
| t04 | LIGHT 回调 | ✅ 20 TPS |
| t05 | 返回点 hook | ✅ 28/28 |
| t06 | 字段读写 | ✅ 3000 次/秒 |
| t07 | 栈局部变量 | ✅ 18 槽位 |
| t08 | 多回调链 | ✅ priority 排序 |
| t08d | 长跑稳定性 | ✅ 30 秒 601 次 |
| t09 | 崩溃隔离 | ✅ 60 次崩溃游戏没死 |
| t10 | 运行期数据表 | ✅ 31,407 条 |
| t11 | 多实例 | ✅ 14/14 |
| t12 | 调用重定向 | ✅ 安装+卸载 |
| t13 | 常量修改 | ✅ 60 次/3 秒 |

---

## 文档

- [API 手册](docs/api/Home.md) — 完整 API 参考
- [快速上手](docs/QUICKSTART.md) — 5 分钟跑通
- [偏移采集](docs/OFFSET_GUIDE.md) — 换版本必读
- [验证报告](docs/REPORT.md) — 完整能力与缺陷清单

---

## 许可

MIT License，见 [LICENSE](LICENSE)。

**仅供学习研究与单机 Mod 开发。禁止用于联机作弊。**
