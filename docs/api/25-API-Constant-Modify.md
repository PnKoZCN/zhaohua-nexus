# API：常量修改

改函数内编码的常量值 —— 对标 Mixin `@ModifyConstant`。

---

## 一、核心 API

### 1.1 `zh_const_count()`

```c
int zh_const_count(const char* funcName, uint32_t value);
```

**作用**：查某函数内等于 `value` 的常量出现次数。

**示例**：

```c
int n = zh_const_count("LocalPlayer_normalTick", 16);
LOG_I(LOG_HOOK, "常量 16 出现 %d 次", n);
```

**实测**（`LocalPlayer_normalTick`）：

```
常量 16    出现 28 次
常量 960   出现 2 次
常量 100   出现 1 次
常量 8     出现 21 次
常量 4     出现 2 次
常量 2     出现 2 次
常量 1     出现 20 次
常量 0     出现 30 次
常量 3     出现 2 次
```

---

### 1.2 `ii_const_addr()`

```c
int ii_const_addr(const char* funcName, uint32_t value, int nth, unsigned long* outAddr);
```

**作用**：查第 `nth` 个常量的**绝对地址**。

**示例**：

```c
unsigned long addr = 0;
int rc = ii_const_addr("LocalPlayer_normalTick", 16, 0, &addr);
if (rc == 0) {
    LOG_I(LOG_HOOK, "第 0 个常量 16 在 0x%lx (func+0x%lx)",
          addr, addr - zh_find("LocalPlayer_normalTick"));
}
```

---

### 1.3 `zh_modify_const()`

```c
int zh_modify_const(const char* funcName, uint32_t value, int nth,
                    ii_callback cb, void* user);
```

**作用**：在常量指令处插桩。

**示例**：

```c
static void on_const(ii_ctx* c, void* user) {
    // 在这里改寄存器，实现常量替换
}

zh_modify_const("LocalPlayer_normalTick", 16, 0, on_const, NULL);
```

**注意**：`zh_modify_const` **只是插桩**，改值需要你在回调里操作寄存器。

---

### 1.4 `ii_install_at_const()`

```c
int ii_install_at_const(const char* funcName, uint32_t value, int nth,
                        ii_callback cb, void* user);
```

**底层版本**（`zh_modify_const` 内部调用它）。

---

## 二、常量指令长什么样

### 2.1 ARM64 立即数指令

| 指令 | 编码 | 用途 |
|---|---|---|
| `movz xN, #imm16, lsl #shift` | `0xD2800000 \| (imm16<<5) \| (shift/16<<21) \| N` | 装载 |
| `movk xN, #imm16, lsl #shift` | `0xF2800000 \| ...` | 拼接 |
| `movn xN, #imm16, lsl #shift` | `0x92800000 \| ...` | 反码 |
| `cmp xN, #imm12`（`subs`） | `0xF1000000 \| (imm12<<10) \| ...` | 比较 |
| `add xN, xM, #imm12` | `0x91000000 \| (imm12<<10) \| ...` | 加 |

### 2.2 数据表记录

```c
// const 表条目
struct {
    uint16_t funcIdx;   // 所属函数（constfunc 表索引）
    uint8_t  kind;      // 0=cmp 1=mov 2=addsub
    uint8_t  width;     // 1/2/4/8 字节
    uint32_t insnOff;   // 相对函数入口
    uint32_t value;     // 常量值
}
```

---

## 三、改寄存器的正确方法

### 3.1 你需要知道指令类型

**`kind` 字段告诉你是哪类**：

| kind | 指令类型 | 改哪个寄存器 |
|---|---|---|
| 0 | `cmp` / `subs` | 被比较的寄存器 |
| 1 | `movz` / `movk` | 目标寄存器 |
| 2 | `add` / `sub` | 目标寄存器 |

### 3.2 示例：改 `cmp X, #16` 为 `cmp X, #100`

**原指令**：`cmp x19, #16`

**插桩点**：该指令地址

**回调**：

```c
static void on_const(ii_ctx* c, void* user) {
    // cmp 之前，x19 里是原值
    // 插桩后，ii_common 已保存所有寄存器
    // 但你不能直接改"比较结果"，只能改被比较的寄存器值

    // 方法 A：改寄存器值（把 x19 改成 -84，使得 x19-16 = 100-16 的语义变了）
    // 这很绕，不推荐

    // 方法 B：用 zh_hook 在函数入口改
    // 更清晰，见下方
}
```

**推荐做法**：**不要在常量指令处改，而是在函数入口用更直接的方式**。

### 3.3 更好的方案：用 `zh_hook` 改参数

```c
// 如果常量是"最大血量 20"，通常函数原型是：
//   Player_setMaxHealth(int maxHp) { if (maxHp > 20) maxHp = 20; }
// 那么在入口改参数即可

static void on_set_max_hp(ii_ctx* c, void* user) {
    (void)user;
    int32_t hp = (int32_t)c->x[1];     // 第一个参数
    if (hp <= 20) {
        c->x[1] = 100;                  // 改成 100
    }
}

zh_hook("Player_setMaxHealth", on_set_max_hp, NULL);
```

**这样更清晰，也更稳定。**

---

## 四、真正的常量修改场景

### 4.1 场景：函数内部硬编码的倍数

```asm
SomeFunc:
    ...
    mov  w1, #16          ← 这里是常量 16
    mul  w0, w0, w1
    ...
```

**要在 `mul` 之前把 16 改成 8**：

**方法：在常量指令处插桩，改 w1**

```c
static void on_const(ii_ctx* c, void* user) {
    // 原指令是 "mov w1, #16"
    // 插桩点在它之前（ii_install_at_const 是在这条指令【处】插桩）
    // 所以此刻 w1 还没被写入

    // 我们需要知道 mov 之后 w1 的值 → 16
    // 要改成 8，可以在插桩点之后覆盖，但 hook 机制不支持"之后"

    // 实际做法：用地址 hook 在 mov 的下一条指令处插桩
    unsigned long mov_addr = ...;
    ii_install(mov_addr + 4, on_set_w1, NULL);
}

static void on_set_w1(ii_ctx* c, void* user) {
    (void)user;
    c->x[1] = 8;    // 覆盖 w1（x1 的低 32 位）
}
```

### 4.2 更简单的：直接在 `ii_dispatch` 的 ctx 里改

**`zh_modify_const` 的语义是"在该常量指令处插桩"，**此时该指令还没执行**。**

**所以你能做的是：**
- 改其他寄存器（间接影响）
- cancel（跳过整个函数体，一般不合适）

**要真正"改这条指令"**，需要：
1. 找到常量指令地址
2. 算出它的下一条指令地址
3. 在下一条指令处 `ii_install`
4. 在回调里改目标寄存器

**或者**：直接在内存里改那条指令的立即数（更暴力，也更有效）：

```c
// 找到 movz w1, #16 的指令地址
unsigned long addr = 0;
ii_const_addr("SomeFunc", 16, 0, &addr);

// 读原指令
uint32_t orig = *(volatile uint32_t*)addr;

// 改立即数：movz w1, #16 → movz w1, #8
uint32_t patched = (orig & ~(0xFFFFu << 5)) | (8u << 5);

// 写入（需要 mprotect）
make_writable(addr, 4);
*(volatile uint32_t*)addr = patched;
__builtin___clear_cache((char*)addr, (char*)addr + 4);
make_readable(addr, 4);
```

**这更直接，但破坏了数据表与实际代码的一致性**（卸载时恢复要注意）。

---

## 五、实测案例

### 5.1 t13 的验证

**目标**：`LocalPlayer_normalTick` 的常量 16

```c
// 1. 查常量
int cnt = zh_const_count("LocalPlayer_normalTick", 16);
LOG_I(LOG_HOOK, "常量 16 出现 %d 次", cnt);   // 实测 28 次

// 2. 找最早执行的
unsigned long best_off = ~0UL;
uint32_t pick = 0;
for (uint32_t v = 0; v <= 128; v++) {
    for (int n = 0; n < 8; n++) {
        unsigned long a = 0;
        if (ii_const_addr("LocalPlayer_normalTick", v, n, &a) != 0) break;
        if (a < best_off) { best_off = a; pick = v; }
    }
}
LOG_I(LOG_HOOK, "最早执行的常量: %u @+0x%lx", pick, best_off - func_addr);

// 3. 插桩
ii_install_at_const("LocalPlayer_normalTick", pick, 0, on_const, NULL);

// 4. 观察
sleep(3);
LOG_I(LOG_HOOK, "命中 %lu 次", g_hits);   // 实测 60 次 / 3 秒 = 20 TPS
```

**结果**：`value=16 @+0x1c`，命中 60 次/3 秒 = 20 TPS ✅

---

## 六、枚举函数的所有常量

```c
static void dump_constants(const char* func) {
    unsigned long func_addr = zh_find(func);
    if (!func_addr) return;

    // 遍历 const 表
    int nfunc = zh_tbl_count(ZH_TBL_CONSTFUNC);
    int target_idx = -1;

    for (int i = 0; i < nfunc; i++) {
        const char* nm = zh_tbl_constfunc(i);
        if (nm && strcmp(nm, func) == 0) { target_idx = i; break; }
    }
    if (target_idx < 0) return;

    LOG_I(LOG_HOOK, "=== %s 的常量 ===", func);

    int n = zh_tbl_count(ZH_TBL_CONST);
    for (int i = 0; i < n; i++) {
        int fi, kind, width; uint32_t insnOff, value;
        if (zh_tbl_const(i, &fi, &kind, &width, &insnOff, &value) != 0) continue;
        if (fi != target_idx) continue;

        LOG_I(LOG_HOOK, "  +0x%-6x value=%-8u kind=%d width=%d",
              insnOff, value, kind, width);
    }
}
```

---

## 七、注意事项

| 事项 | 说明 |
|---|---|
| `value` 必须精确匹配 | 差一就不匹配 |
| 同一 value 可能多处 | 用 `nth` 区分 |
| 常量是**指令编码**，不是数据 | 改它需要改指令 |
| 插桩点在**该指令处** | 指令还没执行 |
| 数据表可能过期 | 版本更新后重新采集 |

---

## 八、与 Mixin 对比

| Mixin | Nexus |
|---|---|
| `@ModifyConstant(constant = @Constant(intValue = 16))` | `zh_modify_const("func", 16, 0, cb, user)` |
| 直接返回新值 | 需手工改寄存器 |
| 编译器匹配指令 | 数据表记录指令 |
| 类型安全 | 手工指定 |

**Mixin 的简洁性来自编译器帮你找指令并生成替换代码。**

**Nexus 把这一步交给了数据表 + 你的回调。**

---

## 下一步

- [调用重定向](26-API-Call-Redirect)
- [返回值读写](28-API-Return-Value)
- [数据表查询](31-API-Data-Table)
