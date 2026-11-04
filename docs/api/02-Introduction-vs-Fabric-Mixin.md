# 与 Fabric Mixin 对照

如果你写过 Fabric Mixin，这篇帮你**在 10 分钟内建立映射**。

---

## 一、总览对照

| Mixin | Nexus | 说明 |
|---|---|---|
| `@Mixin(Target.class)` | 无对应 | Nexus 直接指定函数名，不需"目标类"概念 |
| `@Inject(at = HEAD)` | `zh_hook()` | 函数入口插桩 |
| `@Inject(at = RETURN)` | `zh_hook_ret()` | 返回点插桩 |
| `@Inject(at = TAIL)` | `zh_hook_tail()` | 函数尾部 |
| `@Inject(cancellable = true)` | `ii_cancel(c)` | 取消执行 |
| 多个 `@Inject` | `ii_add_callback()` | 回调链 |
| `@Accessor` | `zh_field_get/set()` | 字段读写 |
| `@Invoker` | `ii_redirect_call()` / 自定义 | 调用私有方法 |
| `@ModifyConstant` | `zh_modify_const()` | 改常量 |
| `@ModifyVariable` | `zh_local_get/set()` | 改局部变量 |
| `@Redirect` | `zh_redirect()` | 调用重定向 |
| `@Overwrite` | 不支持 | 见下方说明 |
| `@Shadow` | 不需要 | 直接按偏移访问 |
| `mixins.json` 配置 | 数据表 `zh_db_full.bin` | 偏移信息 |
| `CallbackInfo` | `ii_ctx*` | 上下文 |
| `priority` | `priority` | 语义相同 |

---

## 二、逐项详解

### 2.1 `@Inject(at = HEAD)` → `zh_hook()`

**Mixin**
```java
@Mixin(PlayerEntity.class)
public class PlayerMixin {
    @Inject(method = "tick", at = @At("HEAD"))
    private void onTick(CallbackInfo ci) {
        System.out.println("Player ticked");
    }
}
```

**Nexus**
```c
static void on_tick(ii_ctx* c, void* user) {
    // c->x[0] = this 指针
    LOG_I(LOG_HOOK, "Player ticked");
}

zh_hook("LocalPlayer_normalTick", on_tick, NULL);
```

**差异**：
- Mixin 通过类名+方法名定位；Nexus 只用**函数名**（数据表里已有映射）
- Mixin 的 `this` 通过字段访问；Nexus 用 `c->x[0]`

---

### 2.2 `@Inject(at = RETURN)` → `zh_hook_ret()`

**Mixin**
```java
@Inject(method = "getHealth", at = @At("RETURN"), cancellable = true)
private void onGetHealth(CallbackInfoReturnable<Float> cir) {
    cir.setReturnValue(20.0f);
}
```

**Nexus**
```c
static void on_health(ii_ctx* c, void* user) {
    zh_set_ret_float(c, 20.0f);
}

zh_hook_ret("Actor_getHealth", on_health, NULL);
```

**差异**：
- Mixin 的 `CallbackInfoReturnable<T>` 泛型安全；Nexus 用 `zh_set_ret_*` 手工指定类型
- Nexus 需要数据表里有该函数的返回点记录（`retpoint` 表）

---

### 2.3 多个 `@Inject` → 回调链

**Mixin**
```java
@Mixin(PlayerEntity.class)
public class MixinA {
    @Inject(method = "tick", at = @At("HEAD"), priority = 300)
    private void a(CallbackInfo ci) { /* 先执行 */ }
}

@Mixin(PlayerEntity.class)
public class MixinB {
    @Inject(method = "tick", at = @At("HEAD"), priority = 100)
    private void b(CallbackInfo ci) { /* 后执行 */ }
}
```

**Nexus**
```c
zh_hook("LocalPlayer_normalTick", cb_a, NULL);              // priority 0
ii_add_callback(addr, cb_high, NULL, 300);                  // 先执行
ii_add_callback(addr, cb_low, NULL, 100);                   // 后执行

// 执行顺序：300 → 100 → 0
```

**差异**：
- Mixin 靠注解的 `priority` 排序；Nexus 显式传数字（大的先跑）
- 验证：`ii_callback_count(addr)` 返回链长

---

### 2.4 `@Accessor` → 字段读写

**Mixin**
```java
@Mixin(PlayerEntity.class)
public interface PlayerAccessor {
    @Accessor("health")
    float getHealth();
    @Accessor("health")
    void setHealth(float value);
}
```

**Nexus**
```c
// 需要知道字段偏移（从数据表查）
static void my_cb(ii_ctx* c, void* user) {
    void* p_this = (void*)c->x[0];

    // 读：base = x0, 偏移 0x250, 8 字节
    int64_t level = zh_field_get(c, 0, 0x250, 8);

    // 写
    zh_field_set(c, 0, 0x250, 8, (int64_t)new_level);
}
```

**差异** —— **这是最大的差别**：

| Mixin | Nexus |
|---|---|
| 用字段**名字** | 用字段**偏移** |
| 编译期解析 | 需查数据表 |
| 版本更新自动适配 | 版本更新需重采集 |

**查偏移的方法**：
```c
// 遍历 fieldacc 表
for (int i = 0; i < zh_tbl_count(ZH_TBL_FIELDACC); i++) {
    const char* fn; uint16_t off; int base, size, wr;
    zh_tbl_fieldacc(i, &fn, &off, &base, &size, &wr);
    if (strcmp(fn, "Actor_getLevel") == 0) {
        // off = 0x250, base = 0, size = 8
    }
}
```

---

### 2.5 `@ModifyConstant` → `zh_modify_const()`

**Mixin**
```java
@ModifyConstant(method = "tick", constant = @Constant(intValue = 16))
private int modifyConstant(int original) {
    return 100;   // 把 16 改成 100
}
```

**Nexus**
```c
static void on_const(ii_ctx* c, void* user) {
    // 回调在该常量指令处执行
    // 改寄存器实现替换（具体改哪个寄存器看指令编码）
    c->x[0] = 100;
}

// 在 "LocalPlayer_normalTick" 里找值为 16 的第 0 个常量指令
zh_modify_const("LocalPlayer_normalTick", 16, 0, on_const, NULL);
```

**差异**：
- Mixin 直接返回新值；Nexus 需手工改正确的寄存器
- Nexus 需先确认常量存在：`zh_const_count("func", 16)`

---

### 2.6 `@Redirect` → `zh_redirect()`

**Mixin**
```java
@Redirect(method = "getStack", at = @At(value = "INVOKE",
          target = "Lnet/minecraft/item/ItemStack;isEmpty()Z"))
private boolean redirectIsEmpty(ItemStack stack) {
    return false;   // 永远返回 false
}
```

**Nexus**
```c
static void* on_call(ii_ctx* c, void* user) {
    // 返回 NULL = 走原实现
    // 返回函数地址 = 跳到你的实现
    return (void*)my_is_empty;
}

zh_redirect("ContainerScreenController_getStackFromModel",
            "ItemStack_isNull",
            on_call, NULL);
```

**差异**：
- Mixin 直接写替换方法；Nexus 返回函数指针
- Nexus 返回 `NULL` 表示"走原目标"

---

### 2.7 `@Inject(cancellable = true)` → `ii_cancel()`

**Mixin**
```java
@Inject(method = "tick", at = @At("HEAD"), cancellable = true)
private void onTick(CallbackInfo ci) {
    if (shouldSkip()) ci.cancel();
}
```

**Nexus**
```c
static void on_tick(ii_ctx* c, void* user) {
    if (should_skip()) {
        ii_cancel(c);   // 跳过函数体
    }
}
```

**限制**：`ii_cancel` 只在**函数入口**站点有效。在返回点 cancel 会破坏栈帧。

---

### 2.8 `@ModifyVariable` → `zh_local_get/set()`

**Mixin**
```java
@ModifyVariable(method = "attack", at = @At("HEAD"), ordinal = 0)
private int modifyDamage(int damage) {
    return damage * 2;
}
```

**Nexus**
```c
// 先查栈槽位
static void on_attack(ii_ctx* c, void* user) {
    // 读 sp + 0x8 处的 4 字节局部变量
    int64_t dmg = zh_local_get(c, 0x8, 4);

    // 写回
    zh_local_set(c, 0x8, 4, dmg * 2);
}

zh_hook("Actor_attack", on_attack, NULL);
```

**差异**：
- Mixin 用 `ordinal` 指定"第几个同类型局部变量"；Nexus 用**栈偏移**
- 需先用 `ii_local_slot()` 枚举确定偏移

```c
int n = ii_local_count("Actor_attack");
for (int i = 0; i < n; i++) {
    uint16_t spOff; uint8_t size; uint32_t insnOff;
    ii_local_slot("Actor_attack", i, &spOff, &size, &insnOff);
    LOG_I(LOG_LOCAL, "槽[%d] sp+0x%x %d 字节", i, spOff, size);
}
```

---

## 三、Mixin 有而 Nexus 没有

### 3.1 `@Overwrite`

**Mixin**
```java
@Overwrite
public void tick() {
    // 完全替换原方法实现
}
```

**Nexus 不支持。**

**替代方案**：
```c
// 1. 在入口 cancel
static void on_tick(ii_ctx* c, void* user) {
    // 你的实现
    ii_cancel(c);   // 跳过原函数体
}

// 2. 手工设置返回值（如果函数有返回值）
static void on_get_health(ii_ctx* c, void* user) {
    ii_cancel(c);
    zh_set_ret_float(c, 20.0f);
}
```

**限制**：`ii_cancel` 只跳过函数体，不处理 callee-saved 寄存器恢复，只能在入口用。

---

### 3.2 `@Shadow`

**Mixin 需要 `@Shadow` 声明要访问的字段/方法。**

**Nexus 不需要** —— 直接用偏移访问：

```c
void* p = (void*)c->x[0];
int64_t field = zh_field_get(c, 0, 0x250, 8);
```

**代价**：没有编译期检查，偏移写错会在运行期读到垃圾。

---

### 3.3 `@Invoker`

**Mixin**
```java
@Invoker("privateMethod")
void callPrivateMethod();
```

**Nexus**：没有直接对应。可用 `ii_redirect_call()` 拦截调用，或手工构造 `bl` 指令跳转。

---

### 3.4 类型安全的 `CallbackInfoReturnable<T>`

**Mixin** 的泛型保证 `cir.setReturnValue(20.0f)` 类型正确。

**Nexus** 需手工指定：
```c
zh_set_ret_int(c, 42);        // int
zh_set_ret_float(c, 3.14f);   // float
zh_set_ret_ptr(c, ptr);       // 指针
zh_set_ret_double(c, 2.718);  // double
```

**类型不匹配不会报错，但会写错寄存器** → 静默失败。

**建议**：用 `ii_ret_kind(addr)` 先查返回类型。

```c
if (ii_ret_kind(addr) == II_RET_FLOAT) {
    zh_set_ret_float(c, 20.0f);
}
```

---

## 四、Nexus 有而 Mixin 没有

### 4.1 崩溃隔离

```c
zh_hook_guarded("risky_func", my_cb, NULL);
// 回调崩溃 → 被捕获，游戏不崩
long n = zh_guard_caught();
```

Mixin 里回调抛异常会传播到游戏逻辑。

### 4.2 运行期数据表

```c
zh_db db;
zh_db_load(&db, "/path/to/db.bin");
zh_tbl_attach(&db);   // 不重编译就换数据
```

Mixin 的映射信息编译进 jar，换版本要重新构建。

### 4.3 多实例

```c
ii_engine* e = ii_engine_create();
ii_engine_install(e, addr, cb, user);
ii_engine_destroy(e);   // 只撤自己那份
```

Mixin 的回调属于 Mixin 类，无法按业务模块精确卸载。

### 4.4 栈局部变量直接枚举

```c
int n = ii_local_count("func");
for (int i = 0; i < n; i++) ii_local_slot("func", i, ...);
```

Mixin 的 `@Local` 只能按类型+序号匹配。

---

## 五、迁移速查表

| 你熟悉的 Mixin 写法 | Nexus 写法 |
|---|---|
| `@Inject(at=HEAD)` | `zh_hook("func", cb, NULL)` |
| `@Inject(at=RETURN)` | `zh_hook_ret("func", cb, NULL)` |
| `cir.setReturnValue(v)` | `zh_set_ret_float(c, v)` / `_int` / `_ptr` |
| `ci.cancel()` | `ii_cancel(c)` |
| `@Accessor("hp")` | `zh_field_get(c, base, off, size)` |
| `@ModifyConstant` | `zh_modify_const("func", val, nth, cb, NULL)` |
| `@Redirect` | `zh_redirect("host", "target", cb, NULL)` |
| `@ModifyVariable` | `zh_local_get/set(c, spOff, size)` |
| `priority = N` | `ii_add_callback(addr, cb, u, N)` |
| `this` | `c->x[0]` |
| 第一个参数 | `c->x[1]` |
| 第二个参数 | `c->x[2]` |

---

## 六、心法差异

| 维度 | Mixin | Nexus |
|---|---|---|
| **定位方式** | 类 + 方法名 | 函数名 → 偏移 |
| **类型安全** | 编译期强类型 | 运行期手工指定 |
| **版本适配** | 自动（重映射） | 手工（重采集偏移） |
| **回调位置** | 由工具计算出所有匹配点 | 需明确指定入口/返回点/指令 |
| **调试** | 有完整栈追踪 | 只有 `wd_log` + 计数 |

**一句话总结**：

> **Mixin 是"声明式 + 编译器帮你算"；Nexus 是"声明式 + 数据表帮你算"。**
> **代价在于：Nexus 的数据表需要人工维护。**

---

## 七、下一步

- [第一个 Hook](03-Getting-Started-First-Hook) — 动手
- [偏移采集方法](50-Guide-Offset-Collection) — 理解数据表怎么来的
- [常见陷阱](52-Guide-Pitfalls) — 别踩坑
