# ZhaoHua Nexus API 手册

**面向 NetEase Minecraft Bedrock 的原生 Hook 框架**

---

## 📖 阅读路线

### 我是新手

1. [为什么用 Nexus](01-Introduction-Why-Nexus) — 了解它能做什么
2. [与 Fabric Mixin 对照](02-Introduction-vs-Fabric-Mixin) — 如果你写过 Mixin
3. [第一个 Hook](03-Getting-Started-First-Hook) — 5 分钟跑通
4. [构建与注入](04-Getting-Started-Build-Inject) — 部署到设备

### 我要查 API

| 主题 | 文档 |
|---|---|
| 生命周期 | [20-API-Lifecycle](20-API-Lifecycle) |
| 函数查找 | [21-API-Find-Function](21-API-Find-Function) |
| Hook 安装 | [22-API-Hook-Install](22-API-Hook-Install) |
| 多回调链 | [23-API-Multi-Callback](23-API-Multi-Callback) |
| 字段访问 | [24-API-Field-Access](24-API-Field-Access) |
| 常量修改 | [25-API-Constant-Modify](25-API-Constant-Modify) |
| 调用重定向 | [26-API-Call-Redirect](26-API-Call-Redirect) |
| 栈局部变量 | [27-API-Stack-Local](27-API-Stack-Local) |
| 返回值读写 | [28-API-Return-Value](28-API-Return-Value) |
| 多实例 | [29-API-Multi-Instance](29-API-Multi-Instance) |
| 崩溃隔离 | [30-API-Crash-Isolation](30-API-Crash-Isolation) |
| 数据表查询 | [31-API-Data-Table](31-API-Data-Table) |
| 日志系统 | [32-API-Logging](32-API-Logging) |

### 我要深入原理

1. [站点与回调链](10-Core-Site-And-Chain)
2. [指令改写原理](11-Core-Instruction-Rewrite)
3. [内存池与外壳](12-Core-Memory-Pool)

### 我要维护数据表

1. [偏移采集方法](50-Guide-Offset-Collection)
2. [版本更新流程](51-Guide-Version-Update)
3. [数据表格式](42-Reference-Data-Format)

### 我遇到问题

- [常见陷阱](52-Guide-Pitfalls) ★ 强烈建议先看
- [错误码](40-Reference-Error-Codes)
- [FAQ](90-FAQ)

---

## ⚠️ 三条铁律（先记住这个）

### 1. 回调里禁止耗时操作

回调运行在**游戏线程**上。Android ANR 阈值 **5 秒**。

```c
// ❌ 禁止
static void cb(ii_ctx* c, void* u) {
    save_to_disk();   // 文件 I/O
    sleep(1);         // 直接 ANR
}

// ✅ 正确：只入队
static void cb(ii_ctx* c, void* u) {
    queue_push(&g_q, snapshot(c));
}
```

引擎在超过 50 ms 时告警 `[SLOW]`。

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

### 3. 开发调试期：一次只注入一个库

**这不是框架限制。** 框架支持多库共存（单站点最多 32 个库、共享 8192 个站点，见 [多库共存](29-API-Multi-Instance)）。

这是**调试纪律** —— 出问题时能确定是谁引起的。

注入 → 验证 → 卸载 → 再注入下一个。验证通过后可同时加载多个库。

---

## 🗺️ 能力速查

| 你想做的事 | 用哪个 API | Mixin 对应 |
|---|---|---|
| 在函数开头插代码 | `zh_hook()` | `@Inject` |
| 在函数 return 前插代码 | `zh_hook_ret()` | `@Inject(at = RETURN)` |
| 同一函数挂多个回调 | `zh_hook()` + `ii_add_callback()` | 多个 `@Inject` |
| 读写对象字段 | `zh_field_get/set()` | `@Accessor` |
| 改函数里的常量 | `zh_modify_const()` | `@ModifyConstant` |
| 把 A 调用改成 B | `zh_redirect()` | `@Redirect` |
| 读局部变量 | `zh_local_get()` | `@Local` |
| 改返回值 | `zh_set_ret_int/float()` | `@Inject(cancellable)` |

---

## 📊 框架数据

```
代码        7,229 行（C 5,802 + 头 1,427）
导出 API    189 个
主机测试    12 / 12 通过
真机验证    15 项能力
数据表      10 张 / 31,407 条
引擎体积    841,984 B
```

**目标游戏**：NetEase MC Bedrock `3.10.5.301327`
**目标库**：`libminecraftpe.so`（323,630,848 字节）

---

## 🔗 相关链接

- [源码仓库](https://github.com/PnKoZCN/zhaohua-nexus)
- [示例模板](https://github.com/PnKoZCN/zhaohua-example-mod)

---

## 📄 许可

**[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)** +
附加条款，不是 MIT。

| 允许 | 禁止 |
|---|---|
| ✅ 使用、修改、分发 | ❌ **商业使用**（NonCommercial） |
| ✅ 个人学习 / 安全研究 / 单机娱乐 | ❌ **闭源分发**（ShareAlike） |
| ✅ 开源项目中引用 | ❌ **移除署名**（Attribution） |
| | ❌ **开发游戏外挂**（附加条款 2） |

**署名格式**：

```
本项目基于 ZhaoHua Nexus 开发
https://github.com/PnKoZCN/zhaohua-nexus
```

完整条款见仓库 `LICENSE` 与 `README` 的「附加条款」一节。
