# ZhaoHua Nexus 框架 —— 完整验证报告

**版本**：1.0
**日期**：2026-11-05
**目标游戏**：NetEase Minecraft Bedrock `3.10.5.301327`（build `3.10.5.301327`）
**目标库**：`libminecraftpe.so`（323,630,848 字节）
**设备**：OnePlus PKX110 / Android 16 (SDK 36) / arm64-v8a / KernelSU

---

## 一、执行摘要

ZhaoHua Nexus 是一套面向 NetEase Minecraft Bedrock 的原生 Hook 框架，对标 Fabric Mixin 的开发体验，提供函数插桩、字段访问、常量修改、调用重定向、栈局部变量读取、多回调链、崩溃隔离与运行期数据表等能力。

**结论：核心能力已全部开发完成并通过真机验证，可用于实际 Mod 开发。**

| 维度 | 结果 |
|---|---|
| 代码规模 | 7,229 行（C 源码 5,802 + 头文件 1,427） |
| 导出 API | 189 个 |
| 主机测试 | **12 / 12 通过** |
| 真机验证 | **15 项能力全部通过** |
| 数据表 | 10 张 / 31,407 条记录 |
| 已知缺陷 | 已修复 13 个（含 8 个致命） |

---

## 二、能力矩阵

### 2.1 已通过真机验证的能力

| # | 测试 | 能力 | 对应 Mixin 注解 | 验证结果 |
|---|---|---|---|---|
| 1 | t01 | 引擎初始化 | — | ✅ 5 池 / 587 KB |
| 2 | t02 | 函数查找 | — | ✅ 124 / 124 |
| 3 | t03 | 装 / 卸 hook | `@Inject` | ✅ 指令改写 + 完整恢复 |
| 4 | t04 | LIGHT 回调 | `@Inject`（轻量） | ✅ 20 TPS |
| 5 | t05 | 返回点 hook | `@Inject(at = RETURN)` | ✅ 28 / 28 |
| 6 | t05c | FULL vs 返回点 | — | ✅ 入口 60 / 返回点 720 |
| 7 | t06 | 字段读写 | `@Accessor` | ✅ 3000 次/秒 |
| 8 | t07 | 栈局部变量 | `@Local` | ✅ 18 槽位，4/4 读取成功 |
| 9 | t08 | 多回调链 | 多个 `@Inject` | ✅ priority 300 → 100 → 0 |
| 10 | t08d | 长跑稳定性 | — | ✅ **30 秒 / 601 次回调，零异常** |
| 11 | t09 | 崩溃隔离 | — | ✅ 回调崩 60 次，游戏未死 |
| 12 | t10 | 运行期数据表 | — | ✅ 31,407 条全部可读 |
| 13 | t11 | 多实例 | — | ✅ **14 / 14 断言通过** |
| 14 | t12 | 调用重定向 | `@Redirect` | ✅ 安装 + 卸载，指令恢复 |
| 15 | t13 | 常量修改 | `@ModifyConstant` | ✅ 60 次 / 3 秒 |

### 2.2 API 功能域分布（189 个导出符号）

| 功能域 | 数量 |
|---|---|
| 生命周期 / 计数 | 21 |
| 函数查找 | 8 |
| Hook 安装 | 31 |
| 多实例 | 8 |
| 调用重定向 | 4 |
| 常量操作 | 8 |
| 字段访问 | 7 |
| 栈局部 / 帧 | 16 |
| 返回点与返回值 | 21 |
| 调用点 | 3 |
| 数据表 | 24 |
| 崩溃隔离 | 4 |
| 信号 / 暂停 / trace | 17 |
| 日志 | 8 |
| 注册表 / 所有者 | 8 |

---

## 三、数据表

### 3.1 表结构

| 表名 | 条目数 | 条目大小 | 用途 |
|---|---|---|---|
| `rettype` | 124 | 16 B | 函数返回类型 |
| `blcall` | 449 | 16 B | 方法调用点 |
| `const` | 26,447 | 12 B | 函数内常量 |
| `frame` | 120 | 12 B | 栈帧信息 |
| `slot` | 547 | 8 B | 栈槽位 |
| `fieldacc` | 716 | 12 B | 字段访问点 |
| `constfunc` | 121 | 4 B | 含常量的函数 |
| `poolhint` | 5 | 8 B | 内存池提示 |
| `retfunc` | 124 | 4 B | 含返回点的函数 |
| `retpoint` | 2,754 | 8 B | 返回点地址 |
| **合计** | **31,407** | — | 367,193 字节 |

### 3.2 双数据源机制

引擎支持两种数据源，通过 `zh_tbl_attach()` 切换：

1. **编译期静态表** —— `data/*.h`（如 `const_db.h` 26,589 行）
2. **运行期 mmap** —— `zh_db_full.bin`（367,193 字节）

```c
zh_db db;
if (zh_db_load(&db, "/path/zh_db_full.bin") == ZH_DB_OK)
    zh_tbl_attach(&db);      // 之后所有查询走 mmap 数据
zh_tbl_attach(NULL);         // 退回静态表
```

### 3.3 数据完整性

打包工具 `tools/verify_db.py` 对 10 张表逐条比对源头文件：
**打包数与源数 100% 一致。**

---

## 四、偏移量采集方法

### 4.1 采集流程

```
原始 libminecraftpe.so
        │
        ├─ 1. 符号表提取 ────────→ 函数名 → 函数偏移
        │     (readelf / nm / IDA 导出)
        │
        ├─ 2. 指令模式扫描 ──────→ 返回点 (ret 指令位置)
        │     (bjump / 反汇编遍历)     调用点 (bl 指令位置)
        │                             常量 (movz/movk 立即数)
        │                             栈槽位 (str/ldr [sp,#off])
        │
        ├─ 3. 字段偏移推导 ──────→ 字段访问点 (ldr/str [xN,#off])
        │     (getter 函数体内的访存模式)
        │
        ├─ 4. 函数名对齐 ────────→ 有符号函数 ↔ 偏移映射
        │     (符号表 + 反汇编交叉验证)
        │
        └─ 5. 打包 ──────────────→ zh_db_full.bin
              (tools/pack_db.py)
                    │
                    └─→ tools/verify_db.py 校验
```

### 4.2 各表采集脚本

| 表 | 采集方式 | 脚本位置 |
|---|---|---|
| `rettype` / `retfunc` | 符号表 + 返回类型推导 | `zh_sym/` |
| `blcall` | 反汇编扫描 `bl` 指令，查目标函数名 | `zh_sym/xmatch2.py` |
| `const` | 扫描 `movz/movk/movn` 立即数 | `zh_sym/constfp.py` |
| `frame` / `slot` | 扫描函数内所有 `[sp,#off]` 存取 | `zh_sym/slotscan.py`、`slottype.py` |
| `fieldacc` | getter 函数体内的 `[xN,#off]` 模式 | `zh_sym/getters_full.h`、`acc*.py` |
| `retpoint` | 遍历函数体，记录 `ret` 指令地址 | `zh_sym/` |
| `poolhint` | 查找 `.bss` / 代码段零填充区 | `zh_sym/` |

### 4.3 版本更新流程（手工）

**引擎不含版本适配逻辑，偏移需手工抓取更新。**

1. 提取新版 `libminecraftpe.so`
2. 重新跑 `zh_sym/` 下的采集脚本
3. 用 `tools/pack_db.py` 重新打包：
   ```bash
   python3 tools/pack_db.py --out data/zh_db_full.bin \
       --ver <新版本号> data/*.h
   ```
4. 用 `tools/verify_db.py` 校验
5. 部署新的 `zh_db_full.bin` 到设备

---

## 五、构建系统

### 5.1 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `ZH_BUILD_TESTS` | OFF | 构建主机侧单元测试 |
| `ZH_BUILD_EXAMPLES` | OFF | 构建示例模块（真机测试用） |
| `ZH_BUILD_STATIC` | OFF | 同时构建静态库 |
| `ZH_USE_ASAN` | OFF | 启用 AddressSanitizer |
| `ZH_WERROR` | OFF | 警告视为错误 |
| `ZH_STRICT` | OFF | 严格警告（-Wall -Wextra） |
| **`ZH_PORTABLE`** | **OFF** | **去 bionic 依赖，可在普通 Linux 加载** |

### 5.2 双平台构建

**Android（生产）**
```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

**x86_64 主机（CI / 回归）**
```bash
cmake -B build-host -DCMAKE_BUILD_TYPE=Release -DZH_BUILD_TESTS=ON
cmake --build build-host -j8
cd build-host && LD_LIBRARY_PATH=. ./tests/test_insn   # 等 12 个
```

### 5.3 `ZH_PORTABLE` 的作用

Android NDK 编译的 `.so` 引用 10 个 bionic 专有符号，普通 Linux 无法 `dlopen`：

```
__errno  __register_atfork  __system_property_get
__memcpy_chk  __write_chk  __strchr_chk  __strlen_chk
__strrchr_chk  __vsnprintf_chk  __open_2
```

开启 `ZH_PORTABLE` 后：
- `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0` → 消除 7 个 `*_chk`
- `src/common/portable.c` 提供 `__errno` / `__register_atfork` / `__system_property_get`

**结果：全部消除，可在 Alpine/musl aarch64 上加载运行**（已在 QEMU 虚拟机验证）。

---

## 六、质量保证体系

### 6.1 四层校验流程

```
[层 1a] 源码审计          _fullaudit.py     100+ 项
[层 1a2] 汇编审计         _asm_audit.py     编码/偏移/寄存器完整性
[层 1b] Python 校验       _pyverify_all.py  API 声明 + 数据一致性
[层 2]  二进制校验        readelf           页对齐/TLS/TLSDESC/VERDEF/TEXTREL/GNU_STACK
[层 3]  注入前进程校验    _precheck4.py     11 项
[层 4]  真机验证          examples/steps/   27 个测试
```

### 6.2 主机测试（12/12 通过）

| 测试 | 覆盖内容 |
|---|---|
| `test_insn` | ARM64 指令编码/解码 |
| `test_datatable` | 数据表完整性 |
| `test_strerror` | 错误码映射 |
| `test_layer` | 门面层 API |
| `test_chain` | 回调链增删改 |
| `test_guard` | 崩溃隔离 |
| `test_engine` | 多实例 |
| `test_typed` | 类型化返回值 |
| `test_sites` | 站点表边界 |
| `test_dispatch` | 调度逻辑 |
| `test_db` | 运行期数据表加载 |
| `test_tbl` | 数据表查询 |

### 6.3 关键校验项示例

**汇编审计**（`_asm_audit.py`）：
```
✅ ldr x16,#8 编码 0x58000050，imm19=2，偏移 8 字节
✅ ii_common 栈分配 0x320，所有偏移合法
✅ sp 平衡：shell(-16) + common(-0x320) + trampoline(+0x330) = 0
✅ 除 x16/x17 外全部 x 寄存器恢复
✅ q0-q31 全部保存/恢复
✅ trampoline 各模式长度 ≤ 槽位容量
```

**二进制审计**：
```
页对齐 0x4000（Android 15+ 要求）
TLS 段 0 / TLSDESC 重定位 0（musl 会引入）
VERDEF 0 / TEXTREL 0（支持 W^X）
GNU_STACK = RW（栈不可执行）
```

---

## 七、已修复的缺陷清单

### 7.1 致命缺陷（8 个）

| # | 缺陷 | 后果 | 修复 |
|---|---|---|---|
| 1 | `SHELL_SIZE` = 64 不足 | trampoline 条件分支模式溢出 12 字节，踩相邻槽位 shell | 改为 128 |
| 2 | `chain_remove` 直接 `free` | 热路径 use-after-free | 改懒删除（`alive=0`） |
| 3 | `ii_uninstall_all` 栈上 2 MB 数组 | 游戏工作线程爆栈 | 改堆分配 |
| 4 | 暂停状态泄漏 | 全线程停在 `do_signal_stop`，整机卡死 + ANR | 严格配对 + `zh_stop_wake_all` 兜底 |
| 5 | `poolhint` 向游戏内存写 guard | 直接改游戏 `.bss`，几秒后闪退无 tombstone | 按池来源区分，hint 池不写 |
| 6 | `scan_pool_for_target` 把游戏代码段登记为池 | 同上 | 只登记匿名可写段 |
| 7 | `ii_common` 缺 `cbz` 兜底 | `br NULL` 跳地址 0 闪退 | 加 `cbz x16, 9f; ret` |
| 8 | `ii_redirect_dispatch` 返回 NULL | 同上 | 返回 `c->pc` |

### 7.2 重要缺陷（5 个）

| # | 缺陷 | 修复 |
|---|---|---|
| 9 | `pack_db.py` fieldacc 每条少 2 字节 | `<IHBBBB`(10B) → `<IHBBBxxx`(12B) |
| 10 | `ii_engine_uninstall` 撤整个站点 | 加 `owner` 字段，按归属精确摘除 |
| 11 | `uninstall_all` 绕过 owner 过滤 | 改走 `ii_engine_uninstall` |
| 12 | `ii_register_hint_pools` 路径未受控 | 与 `zhreg_add_pool` 的 `noGuard` 配合 |
| 13 | `next_n_clean(addr, 32)` 过度保守 | 改为 `next_n_clean(addr, 4)` |

### 7.3 框架完整性缺陷（4 个）

| # | 缺陷 | 修复 |
|---|---|---|
| 14 | ARM64 汇编无架构守卫 → x86_64 主机构建失败 | 加 `ZH_ARCH_ARM64` 宏 + 桩实现 |
| 15 | `ii_guard_*` 未进导出表 → 测试链接失败 | 重新生成 version script |
| 16 | `test_db` 硬编码 Android 路径 | 改用 `/tmp` 临时文件 |
| 17 | `test_db` 造假文件 12 字节 < `HDR_LEN`(32) | 改为 128 字节 |

---

## 八、关键调试经验

### 8.1 最重要的认知修正

**问题现象**：所有长跑测试（`sleep(2)×15`）在 6 秒内崩溃，20+ 轮调试均未能定位。

**真实原因**：
```c
__attribute__((constructor))
static void run(void) {
    zh_hook(...);
    for (int i = 0; i < 15; i++) { sleep(2); ... }   // ← 阻塞 dlopen 30 秒
}
```

`constructor` 由 `dlopen` **同步执行**。若注入线程恰为游戏主循环/渲染线程，触摸事件 5 秒无人处理 → `Input dispatching timed out` → ANR 闪退。

**ANR 日志证据**：
```
Subject: Input dispatching timed out (... Waited 5000ms for MotionEvent ...)
```

**验证**：
```
t08d（异步写法）: 30 秒 / 601 次回调，稳定       ✅
t11 （异步写法）: 14 项断言全过                ✅
t12 （异步写法）: 安装 + 卸载，20 秒存活        ✅
```

**同一份引擎代码，仅改测试写法就从「必崩」变成「全通」。**

### 8.2 正确写法

```c
// ✅ constructor 立刻返回，工作交给独立线程
static void* worker(void* arg) {
    /* 所有耗时逻辑 */
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
```

---

## 九、使用约束（必须遵守）

### 9.1 回调里禁止耗时操作

回调运行在**被 hook 的游戏线程**上，任何阻塞都会冻结游戏：

| 操作 | 是否允许 |
|---|---|
| 读写内存、原子操作 | ✅ 允许 |
| 调用引擎 API | ✅ 允许 |
| 轻量计算 | ✅ 允许 |
| `malloc` / `free` | ⚠️ 可能阻塞，尽量避免 |
| 文件 I/O | ⚠️ 慢，超 50 ms 会告警 |
| `sleep` / 网络 / 锁等待 | ❌ **禁止** |

**引擎提供 50 ms 阈值告警**：
```
[SLOW] 回调 @0x7c4cfba6c8 耗时 520 ms（阈值 50）。
回调运行在游戏线程上，重操作会冻结游戏，请改为异步。
```

**正确模式 —— 只入队，异步处理**：
```c
static void cb(ii_ctx* c, void* u) {
    queue_push(&g_q, snapshot_of(c));   // 快，只入队
}
// 独立线程消费队列做重活
```

### 9.2 系统 ANR 阈值

Android 输入分发超时为 **5 秒**。任何一次回调阻塞超过此值即触发 ANR。

### 9.3 注入流程

```bash
# 1. 四层校验
python3 _fullaudit.py && python3 _asm_audit.py && python3 _pyverify_all.py
python3 _precheck4.py <pid> <whitelist>

# 2. 注入引擎（只需一次）
./zhinf <pid> libzh_engine.so <mmap_addr> <dlopen_addr>

# 3. 注入业务模块
./zhinf <pid> libyour_mod.so <mmap_addr> <dlopen_addr>

# 4. 验证后立即删除注入的库文件
rm <libdir>/libyour_mod.so
```

---

## 十、架构设计

### 10.1 目录结构

```
zh_engine/
├── CMakeLists.txt              构建定义（7 个选项）
├── CMakePresets.json
├── README.md
├── include/zh/                 7 个头文件，1,427 行
│   ├── instr_hook.h            底层 API（101 个导出）
│   ├── zh_layer.h              门面层 API
│   ├── zh_tbl.h                数据表查询
│   ├── zh_db.h                 运行期数据表
│   ├── zh_typed.h              类型化返回值
│   ├── zhreg.h                 跨探针注册表
│   └── zlog.h                  日志
├── src/                        5,802 行
│   ├── instr_hook.c            3,679 行（核心）
│   ├── zh_layer.c                634 行（门面）
│   ├── zhreg.c                   472 行（共享注册表）
│   ├── zh_tbl.c                  293 行
│   ├── zlog.c                    187 行
│   ├── zh_db.c                   143 行
│   ├── reloc.c                   133 行
│   ├── guard.c                   120 行（崩溃隔离）
│   ├── ctxdump.c                  63 行
│   ├── common/portable.c          29 行（非 Android 兼容）
│   └── arch/arm64/              （预留）
├── data/                       9 个表头 + 2 个 .bin
├── tests/                      12 个主机测试
├── examples/steps/             27 个真机测试
├── tools/                      pack_db.py / verify_db.py
└── cmake/zh_engine.map.txt     189 个导出符号
```

### 10.2 Hook 运行时布局

```
被 hook 函数入口
    │  (4 字节原子写: b shell)
    ▼
shell（28 字节）
    stp  x16, x17, [sp, #-16]!     保存原 x16/x17
    movz x17, #sid                 传站点号
    ldr  x16, #8                   装载 handler
    br   x16
    .quad ii_common                handler 地址
    │
    ▼
ii_common（栈帧 0x320）
    保存 x0-x30, q0-q31, fpsr, fpcr, pstate
    构造 ii_ctx
    bl ii_dispatch                 C 层：遍历回调链
    恢复寄存器
    br trampoline
    │
    ▼
trampoline（最多 44 字节）
    ldr x16, [sp, #0x80]           恢复原 x16
    ldr x17, [sp, #0x88]           恢复原 x17
    add sp, sp, #0x330             恢复 sp
    <原指令>
    b addr+4                       跳回函数继续执行
```

**栈平衡**：`shell(-16) + ii_common(-0x320) + trampoline(+0x330) = 0`

### 10.3 内存池策略

| 池来源 | `kind` | 写 guard | 用途 |
|---|---|---|---|
| `pool_from_mmap_near()` | `own` | ✅ 写 | 自己 mmap 的独占页 |
| `poolhint` 表 | `hint` | ❌ 不写 | 游戏 `.so` 内零填充区（±128MB 内唯一来源） |
| `scan_pool_for_target()` | `anon` / `bss` / `code` | ❌ 不写 | 进程已有映射的补充候选 |

**存活检测**：`own` 池比对 guard 标记；其余用 `mincore()`（只读，无副作用）。

---

## 十一、完成度评估

### 11.1 生产可用 ✅

| 能力 | 就绪度 |
|---|---|
| 函数 hook（入口 / 返回点 / 尾部） | ✅ |
| 多回调链（对标多 `@Inject`） | ✅ |
| 字段读写（对标 `@Accessor`） | ✅ |
| 常量修改（对标 `@ModifyConstant`） | ✅ |
| 调用重定向（对标 `@Redirect`） | ✅ |
| 栈局部变量读取（对标 `@Local`） | ✅ |
| 崩溃隔离 | ✅ |
| 运行期数据表 | ✅ |
| 多实例 | ✅ |
| 日志系统 | ✅ |
| 跨平台构建 | ✅ |

### 11.2 待完善 ⚠️

| 项 | 现状 | 建议 |
|---|---|---|
| 文档 | 160 行 README | 补 API 手册、迁移指南 |
| 数据表更新 | 手工抓偏移 | 自动化采集流水线 |
| 版本适配 | 仅 3.10.5.301327 | 建立版本 → 数据表映射 |
| 性能基准 | 粗略数据 | 系统化 benchmark |
| 其它架构 | 只有 ARM64 | 如有需要再扩展 |

### 11.3 已知限制 🔶

| 限制 | 原因 |
|---|---|
| 依赖 `libminecraftpe.so` 精确版本 | 偏移是硬编码数据 |
| `±128MB` 内无空洞时退 16 字节模式 | `bl` 只能跳 ±128MB |
| 16 字节模式需暂停线程 | 非原子写入 |
| 回调必须轻量 | 运行在游戏线程 |

---

## 十二、交付物清单

```
zh_deliver/
├── REPORT.md                      本报告
├── zh_engine/                     完整源码
│   ├── include/                   头文件
│   ├── src/                       实现
│   ├── data/                      数据表（.h + .bin）
│   ├── tests/                     12 个主机测试
│   ├── examples/steps/            27 个真机测试
│   ├── tools/                     打包 + 校验工具
│   └── cmake/                     导出符号表
├── offset_tools/                  偏移采集脚本
├── audit/                         四层校验脚本
├── docs/                          开发过程文档（23 份）
└── prebuilt/                      预编译产物
```

---

## 十三、结论

**ZhaoHua Nexus 的核心能力已全部开发完成并通过真机验证，可以用于实际 Mod 开发。**

**关键数据**：
- 7,229 行代码，189 个导出 API
- 12/12 主机测试通过
- 15 项能力真机验证通过
- 13 个缺陷已修复（含 8 个致命）
- 30 秒 / 601 次回调长跑稳定

**使用前提**：
1. 目标游戏版本必须匹配 `3.10.5.301327`
2. 回调内禁止耗时操作（50 ms 告警阈值）
3. 测试代码禁止在 `constructor` 里 `sleep`

**下一步建议**：
1. 补全 API 文档与迁移指南
2. 建立偏移采集自动化流水线
3. 补充性能基准测试
