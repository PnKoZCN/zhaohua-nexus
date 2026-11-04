# API：返回值读写

读、改函数返回值 —— 对标 Mixin `@ModifyReturnValue`。

---

## 一、核心 API

### 1.1 按类型读

```c
int64_t zh_get_ret_i64(ii_ctx* c);
int32_t zh_get_ret_i32(ii_ctx* c);
int16_t zh_get_ret_i16(ii_ctx* c);
int8_t  zh_get_ret_i8(ii_ctx* c);
uint64_t zh_get_ret_u64(ii_ctx* c);
float   zh_get_ret_float(ii_ctx* c);
double  zh_get_ret_double(ii_ctx* c);
void*   zh_get_ret_ptr(ii_ctx* c);
```

**作用**：在返回点读 `x0`（或 `q0` 浮点）。

**示例**：

```c
static void on_ret(ii_ctx* c, void* user) {
    (void)user;
    int32_t hp = zh_get_ret_i32(c);
    LOG_I(LOG_HOOK, "返回 %d", hp);
}
```

### 1.2 按类型写

```c
void zh_set_ret_i64(ii_ctx* c, int64_t v);
void zh_set_ret_i32(ii_ctx* c, int32_t v);
void zh_set_ret_i16(ii_ctx* c, int16_t v);
void zh_set_ret_i8 (ii_ctx* c, int8_t  v);
void zh_set_ret_u64(ii_ctx* c, uint64_t v);
void zh_set_ret_float(ii_ctx* c, float v);
void zh_set_ret_double(ii_ctx* c, double v);
void zh_set_ret_ptr(ii_ctx* c, void* v);
```

**示例**：

```c
static void on_health(ii_ctx* c, void* user) {
    (void)user;
    float hp = zh_get_ret_float(c);
    if (hp > 0 && hp < 10.0f) {
        zh_set_ret_float(c, 20.0f);
    }
}
```

---

## 二、返回值在 ctx 的哪里

### 2.1 整数 / 指针 → `x0`

```c
int64_t zh_get_ret_i64(ii_ctx* c) {
    return (int64_t)c->x[0];
}

void zh_set_ret_i64(ii_ctx* c, int64_t v) {
    c->x[0] = (uint64_t)v;
}
```

### 2.2 `int32` → `w0`（`x0` 的低 32 位）

```c
int32_t zh_get_ret_i32(ii_ctx* c) {
    return (int32_t)(c->x[0] & 0xFFFFFFFFu);
}

void zh_set_ret_i32(ii_ctx* c, int32_t v) {
    c->x[0] = (uint64_t)(uint32_t)v;   // ★ 清高 32 位
}
```

**注意**：写 `int32` 时**必须清零高 32 位**。否则高 32 位垃圾会导致调用者截断后得到错误值（或更糟，指针高位不对）。

### 2.3 `bool` → `w0` 的 bit 0

```c
static inline bool zh_get_ret_bool(ii_ctx* c) {
    return (c->x[0] & 0xFF) != 0;
}
static inline void zh_set_ret_bool(ii_ctx* c, bool v) {
    c->x[0] = v ? 1 : 0;
}
```

### 2.4 `float` → `q0.s[0]`

```c
float zh_get_ret_float(ii_ctx* c) {
    float f;
    memcpy(&f, c->q, 4);       // q[0] 的前 4 字节
    return f;
}

void zh_set_ret_float(ii_ctx* c, float v) {
    memcpy(c->q, &v, 4);
}
```

### 2.5 `double` → `d0`

```c
double zh_get_ret_double(ii_ctx* c) {
    double d;
    memcpy(&d, c->q, 8);
    return d;
}

void zh_set_ret_double(ii_ctx* c, double v) {
    memcpy(c->q, &v, 8);
}
```

**注意**：`q[0]` 是 16 字节（128 位），`float` 只用前 4 字节，`double` 用前 8 字节。

---

## 三、实测案例

### 3.1 t05c：FULL 模式 vs 返回点

**目标**：对比函数入口 hook 和返回点 hook 能做什么。

```c
// 入口 hook：返回值还没生成
static void on_entry(ii_ctx* c, void* user) {
    (void)user;
    void* p_this = (void*)c->x[0];    // 有 this
    // 没有返回值
}

// 返回点 hook：返回值已在 x0
static void on_ret(ii_ctx* c, void* user) {
    (void)user;
    int32_t ret = zh_get_ret_i32(c);   // 有返回值
    // 但 p_this 已不在 x0（x0 被返回值覆盖）
}
```

**关键发现**：

| | 入口 hook | 返回点 hook |
|---|---|---|
| `x0` | this | 返回值 |
| 参数 | ✅ 有 | ❌ 被覆盖 |
| 返回值 | ❌ 没有 | ✅ 有 |

**要同时有参数和返回值**：需要自己在入口保存参数。

```c
static void* g_saved_this = NULL;

static void on_entry(ii_ctx* c, void* user) {
    (void)user;
    g_saved_this = (void*)c->x[0];   // 存起来
}

static void on_ret(ii_ctx* c, void* user) {
    (void)user;
    int32_t ret = zh_get_ret_i32(c);
    LOG_I(LOG_HOOK, "this=%p ret=%d", g_saved_this, ret);
}
```

**注意**：`g_saved_this` 是全局的 —— 如果函数被多个线程同时调用，会串。

---

### 3.2 t05c 实测结果

```
函数：ItemStack_isNull
FULL 模式安装：rc=0，回调能读 this
返回点安装：rc=0，回调能读返回值
返回值类型：bool (kind=3)

实测 28 个返回点全部可装 ✅
```

---

## 四、多返回点

### 4.1 查返回点数量

```c
int n = ii_retpoint_count("Actor_getHealth");
LOG_I(LOG_HOOK, "有 %d 个返回点", n);
```

**实测**：

| 函数 | 返回点数 |
|---|---|
| `Actor_getHealth` | 1 |
| `ItemStack_isNull` | 28 |
| `LocalPlayer_normalTick` | 2 |

### 4.2 遍历返回点

```c
for (int i = 0; i < n; i++) {
    unsigned long a = 0;
    if (ii_retpoint_addr("Actor_getHealth", i, &a) == 0) {
        LOG_I(LOG_HOOK, "  [%d] 0x%lx", i, a);
    }
}
```

### 4.3 挂所有返回点

```c
ii_install_at_return("ItemStack_isNull", -1, on_ret, NULL);
```

**实测**：28 个返回点全部装上，工作正常。

---

## 五、按返回类型分类处理

### 5.1 查返回类型

```c
int kind = ii_ret_kind(addr);          // 0=void 1=i64 2=i32 3=bool 4=f32 5=f64 6=ptr ...
const char* name = ii_ret_type_name(addr);   // "bool"
```

**数据表里的 124 个函数都有返回类型记录（`rettype` 表）。**

### 5.2 按类型分发

```c
static void on_ret_generic(ii_ctx* c, void* user) {
    int kind = (int)(uintptr_t)user;

    switch (kind) {
    case 1: {
        int64_t v = zh_get_ret_i64(c);
        if (v < 0) zh_set_ret_i64(c, 0);
        break;
    }
    case 2: {
        int32_t v = zh_get_ret_i32(c);
        if (v < 0) zh_set_ret_i32(c, 0);
        break;
    }
    case 4: {
        float v = zh_get_ret_float(c);
        if (v < 0.0f) zh_set_ret_float(c, 0.0f);
        break;
    }
    default: break;
    }
}

// 安装
unsigned long addr = zh_find(fn);
int kind = ii_ret_kind(addr);
ii_install_at_return(fn, -1, on_ret_generic, (void*)(uintptr_t)kind);
```

---

## 六、修改返回值 vs cancel

### 6.1 `ii_cancel()` 在入口

```c
static void on_entry(ii_ctx* c, void* user) {
    if (should_skip(c)) {
        ii_cancel(c);      // 跳过函数体，x0 保持原值（= this）
    }
}
```

**问题**：`x0` 还是 `this`，不是你想要的值。

### 6.2 正确做法：cancel + 设返回值

```c
static void on_entry(ii_ctx* c, void* user) {
    (void)user;
    if (should_skip(c)) {
        zh_set_ret_i32(c, 0);   // 先设返回值
        ii_cancel(c);           // 再 cancel
    }
}
```

**`ii_common` 的 cancel 路径**：

```asm
cbz  w0, 7f
9:  ...                        ; 恢复寄存器
    ldp  x0, x1, [sp, #0x000]  ; ★ 恢复的 x0 就是你改过的值
    ...
    add  sp, sp, #0x330
    add  sp, sp, #0x10
    ret                        ; 直接返回
```

**所以 cancel 时 `x0` 的值会原样返回给调用者。**

---

## 七、返回点 hook 的限制

### 7.1 不能 cancel

**返回点处 cancel 会破坏栈帧**（callee-saved 寄存器未恢复）。

```c
static void on_ret(ii_ctx* c, void* user) {
    ii_cancel(c);   // ★ 危险！不要这样做
}
```

**`ii_dispatch` 里只对入口站点检查 cancel**：

```c
static int ii_dispatch(ii_site* s, ii_ctx* c) {
    ...
    if (s->isRet) return 0;    // 返回点直接放行，不处理 cancel
    ...
}
```

### 7.2 返回点也不能改参数

**`x0..x7` 已经被返回值和被调用过程覆盖了。**

### 7.3 返回点能做什么

| 能 | 不能 |
|---|---|
| 读返回值 | 读参数 |
| 改返回值 | 改参数 |
| 记录调用 | cancel |
| 读 `x0` 指向的对象 | 读 `this` |

---

## 八、浮点返回值的坑

### 8.1 `q0` 的布局

```c
typedef struct {
    uint64_t x[31];      // 0x000 - 0x0F7
    uint64_t sp;         // 0x0F8
    uint64_t pc;         // 0x100
    uint64_t pstate;     // 0x108
    uint8_t  q[32][16];  // 0x110 - 0x30F  ★ 32 个 128 位
    uint32_t fpsr;       // 0x310
    uint32_t fpcr;       // 0x318
} ii_ctx;
```

**`c->q` 是 `uint8_t[32][16]`，`q[0]` 是 16 字节。**

### 8.2 `float` 只用前 4 字节

```c
float zh_get_ret_float(ii_ctx* c) {
    float f;
    memcpy(&f, c->q[0], 4);   // 只读 4 字节
    return f;
}
```

**后 12 字节可能是垃圾**（`ii_common` 保存了整个 `q0`，但 `float` 返回只用了低 32 位）。

### 8.3 `double` 用前 8 字节

```c
double zh_get_ret_double(ii_ctx* c) {
    double d;
    memcpy(&d, c->q[0], 8);
    return d;
}
```

### 8.4 写的时候也只写低字节

```c
void zh_set_ret_float(ii_ctx* c, float v) {
    memcpy(c->q[0], &v, 4);
    // 不要 memset 整个 16 字节 —— 高位的垃圾不影响 float 返回
}
```

---

## 九、完整示例

```c
#include <string.h>
#include <unistd.h>
#include "zh/zh_layer.h"
#include "zh/zlog.h"

static volatile unsigned long g_calls = 0;
static volatile unsigned long g_modified = 0;
static volatile float          g_last_hp = 0;

// 入口：保存 this
static void* g_saved_this = NULL;

static void on_entry(ii_ctx* c, void* user) {
    (void)user;
    g_saved_this = (void*)c->x[0];
}

// 返回点：改血量
static void on_ret(ii_ctx* c, void* user) {
    (void)user;
    __atomic_add_fetch(&g_calls, 1, __ATOMIC_RELAXED);

    float hp = zh_get_ret_float(c);
    __atomic_store_n(&g_last_hp, hp, __ATOMIC_RELAXED);

    if (hp > 0.0f && hp < 10.0f) {
        zh_set_ret_float(c, 20.0f);
        __atomic_add_fetch(&g_modified, 1, __ATOMIC_RELAXED);
    }
}

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;

    const char* fn = "Actor_getHealth";

    // 1. 查返回点
    int n = ii_retpoint_count(fn);
    LOG_I(LOG_HOOK, "%s 有 %d 个返回点", fn, n);

    // 2. 查返回类型
    unsigned long addr = zh_find(fn);
    LOG_I(LOG_HOOK, "返回类型: %s (kind=%d)",
          ii_ret_type_name(addr), ii_ret_kind(addr));

    // 3. 入口 hook（存 this）
    zh_hook(fn, on_entry, NULL);

    // 4. 返回点 hook（改返回值）
    int rc = ii_install_at_return(fn, -1, on_ret, NULL);
    LOG_I(LOG_HOOK, "返回点 hook rc=%d", rc);

    // 5. 观察
    while (1) {
        sleep(5);
        LOG_I(LOG_HOOK, "calls=%lu modified=%lu last_hp=%.1f",
              __atomic_load_n(&g_calls, __ATOMIC_RELAXED),
              __atomic_load_n(&g_modified, __ATOMIC_RELAXED),
              __atomic_load_n(&g_last_hp, __ATOMIC_RELAXED));
    }

    zh_unhook(fn);
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
```

---

## 十、与 Mixin 对比

| Mixin | Nexus |
|---|---|
| `@Inject(at = @At("RETURN"), cancellable = true)` | `ii_install_at_return(fn, -1, cb, user)` |
| `cir.setReturnValue(v)` | `zh_set_ret_xxx(c, v)` |
| `@ModifyReturnValue` | 同上 |
| 编译器保证类型 | 手工选函数 |

**Mixin**：
```java
@Inject(method = "getHealth", at = @At("RETURN"), cancellable = true)
private void onGetHealth(CallbackInfoReturnable<Float> cir) {
    if (cir.getReturnValue() < 10.0f) cir.setReturnValue(20.0f);
}
```

**Nexus**：
```c
static void on_get_health(ii_ctx* c, void* user) {
    (void)user;
    float hp = zh_get_ret_float(c);
    if (hp > 0 && hp < 10.0f) zh_set_ret_float(c, 20.0f);
}
ii_install_at_return("Actor_getHealth", -1, on_get_health, NULL);
```

---

## 十一、注意事项

| 事项 | 说明 |
|---|---|
| 写 `i32` 要清高 32 位 | 否则高位垃圾 |
| 浮点只用 `q[0]` 低字节 | 不要写整个 16 字节 |
| 返回点不能 cancel | 破坏栈帧 |
| 返回点读不到参数 | `x0..x7` 已被覆盖 |
| 多返回点要全挂 | 否则漏路径 |
| `void` 函数无返回值 | 改了也没用 |

---

## 下一步

- [Hook 安装](22-API-Hook-Install)
- [多实例](29-API-Multi-Instance)
- [崩溃隔离](30-API-Crash-Isolation)
