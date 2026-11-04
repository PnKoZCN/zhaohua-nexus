# 为什么用 ZhaoHua Nexus

## 问题：Bedrock 原生库是黑盒

NetEase Minecraft Bedrock 的核心逻辑在 `libminecraftpe.so` 里 —— 一个 323 MB 的闭源 ARM64 原生库。

如果你想改游戏行为（比如让玩家回血更快、让某个物品无敌），传统做法只有：

| 做法 | 局限 |
|---|---|
| 改 APK 重打包 | 签名校验、体积大、无法热更新 |
| 内存搜索改值 | 不稳定、重启失效、难以持久 |
| 反汇编找偏移手工 patch | 每次游戏更新都要重做，极易出错 |

**这些都没有「框架」可言 —— 每个 Mod 都是从头写一遍。**

---

## 对比：Java 侧有 Fabric Mixin

写 Java Edition 的 Mod 时，`@Inject`、`@Redirect`、`@Accessor` 这些注解让开发者能声明式地介入游戏逻辑：

```java
@Mixin(PlayerEntity.class)
public class PlayerMixin {
    @Inject(method = "tick", at = @At("HEAD"))
    private void onTick(CallbackInfo ci) {
        // 玩家每 tick 都会走这里
    }

    @Redirect(method = "getHealth", at = @At("RETURN"))
    private float modifyHealth() {
        return 20.0f;
    }
}
```

**声明式、可组合、有清晰的生命周期。**

**Bedrock 的 C++ 原生层没有这个。**

---

## Nexus 做的事

把 Mixin 的开发体验搬到原生层：

```c
// 在函数开头插代码 —— 对应 @Inject(at = HEAD)
zh_hook("LocalPlayer_normalTick", on_tick, NULL);

// 在 return 前插代码 —— 对应 @Inject(at = RETURN)
zh_hook_ret("Actor_getHealth", on_health, NULL);

// 读字段 —— 对应 @Accessor
int64_t level = zh_field_get(c, 0, 0x250, 8);

// 改常量 —— 对应 @ModifyConstant
zh_modify_const("LocalPlayer_normalTick", 16, 0, on_const, NULL);

// 调用重定向 —— 对应 @Redirect
zh_redirect("Container_getStack", "ItemStack_isNull", on_call, NULL);
```

**你不需要知道指令怎么改、内存怎么分配、回调链怎么维护 —— 只需要知道函数名。**

---

## 核心能力

### 1. 按函数名 Hook

不用记偏移，数据表里有 124 个常用函数：

```c
unsigned long addr = zh_find("Actor_getPos");
if (addr) {
    zh_hook("Actor_getPos", on_getpos, NULL);
}
```

### 2. 三种插入点

| 插入点 | API | 用途 |
|---|---|---|
| 函数开头 | `zh_hook()` | 改参数、读 this |
| 函数返回前 | `zh_hook_ret()` | 改返回值 |
| 常量指令处 | `zh_modify_const()` | 改游戏常量 |

### 3. 多回调链

同一个函数可以挂多个回调，按优先级执行（对标 Mixin 多个 `@Inject`）：

```c
zh_hook("func", cb_low, NULL);              // priority 0
ii_add_callback(addr, cb_mid, NULL, 100);   // priority 100
ii_add_callback(addr, cb_high, NULL, 300);  // priority 300

// 执行顺序：300 → 100 → 0
```

### 4. 崩溃隔离

回调里写错代码不会带崩整个游戏：

```c
zh_hook_guarded("risky_func", my_cb, NULL);

// 每帧检查
if (zh_guard_caught() > 0) {
    LOG_W(LOG_HOOK, "回调崩了 %ld 次", zh_guard_caught());
}
```

### 5. 运行期数据表

偏移数据可以从文件加载，换版本不用重新编译引擎：

```c
zh_db db;
if (zh_db_load(&db, "/path/zh_db_full.bin") == ZH_DB_OK) {
    zh_tbl_attach(&db);       // 之后所有查询走新数据
}
```

### 6. 多实例

不同模块可以独立管理自己的 hook：

```c
ii_engine* my_engine = ii_engine_create();
ii_engine_install(my_engine, addr, cb, user);

// 只撤自己那份，不影响其他模块
ii_engine_destroy(my_engine);
```

---

## 适用场景

### ✅ 适合

| 场景 | 说明 |
|---|---|
| **单机 Mod 开发** | 改数值、加功能、做工具 |
| **游戏行为研究** | 观察函数调用、字段变化 |
| **自动化测试** | hook 关键函数做断言 |
| **性能分析** | 统计函数调用次数与耗时 |

### ❌ 不适合

| 场景 | 原因 |
|---|---|
| **联机作弊** | 违反服务条款，且本框架无网络同步能力 |
| **跨版本通用** | 偏移硬编码，换版本要重新采集 |
| **免 root 使用** | 需要 root 注入 |

---

## 与其他方案对比

| 方案 | 开发体验 | 稳定性 | 版本适配 | 能力上限 |
|---|---|---|---|---|
| 改 APK 重打包 | 差 | 高 | 每次重做 | 只能改资源 |
| 内存搜索改值 | 中 | 低 | 每次重找 | 只能改数据 |
| 手工反汇编 patch | 差 | 中 | 每次重做 | 任意但极慢 |
| **Nexus** | **好（声明式）** | **高** | **只需更新数据表** | **任意函数级** |

---

## 它不做什么

**明确边界，避免误解：**

- ❌ 不提供 Java 层 hook（那是 JNI 的领域）
- ❌ 不提供脚本语言绑定（纯 C API）
- ❌ 不做自动偏移采集（需手工跑脚本）
- ❌ 不做多版本兼容（一个数据表对应一个游戏版本）
- ❌ 不绕过反作弊（本框架不是外挂）

---

## 下一步

- 如果你写过 Mixin → [与 Fabric Mixin 对照](02-Introduction-vs-Fabric-Mixin)
- 如果你只想跑起来 → [第一个 Hook](03-Getting-Started-First-Hook)
- 如果你想了解原理 → [指令改写原理](11-Core-Instruction-Rewrite)
