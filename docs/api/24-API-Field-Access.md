# API：字段访问

读写对象字段 —— 对标 Mixin `@Accessor`。

---

## 一、核心 API

### 1.1 `zh_field_get()`

```c
int64_t zh_field_get(ii_ctx* c, uint8_t argIdx, uint16_t off, uint8_t size);
```

**作用**：从第 `argIdx` 个参数（对象指针）+ 偏移 `off` 处读取 `size` 字节。

**参数**：

| 参数 | 含义 |
|---|---|
| `argIdx` | 0 = `this`（`c->x[0]`），1 = 第一个参数（`c->x[1]`），... |
| `off` | 字段偏移（字节） |
| `size` | 读取宽度：1 / 2 / 4 / 8 |

**返回**：读到的值（有符号扩展）。

**示例**：

```c
static void on_tick(ii_ctx* c, void* user) {
    (void)user;

    // 读 this->某字段（假设偏移 0x250，8 字节指针）
    int64_t level = zh_field_get(c, 0, 0x250, 8);
    if (level) {
        // 用这个 level 指针
    }
}
```

---

### 1.2 `zh_field_set()`

```c
void zh_field_set(ii_ctx* c, uint8_t argIdx, uint16_t off, uint8_t size, int64_t v);
```

**作用**：写入。

```c
zh_field_set(c, 0, 0x250, 8, (int64_t)new_level_ptr);
```

---

### 1.3 便捷宏

```c
#define ZH_THIS_I32(c, off)  ((int32_t)zh_field_get((c), 0, (off), 4))
#define ZH_THIS_I64(c, off)  zh_field_get((c), 0, (off), 8)
#define ZH_THIS_PTR(c, off)  ((void*)(uintptr_t)zh_field_get((c), 0, (off), 8))

#define ZH_ARG_I32(c, i, off) ((int32_t)zh_field_get((c), (i), (off), 4))
#define ZH_ARG_PTR(c, i, off) ((void*)(uintptr_t)zh_field_get((c), (i), (off), 8))
```

**用法**：

```c
void* p_this = ZH_THIS_PTR(c, 0x250);
int32_t hp   = ZH_THIS_I32(c, 0x2a0);
```

---

## 二、怎么找到字段偏移

### 2.1 从 `fieldacc` 表查

```c
for (int i = 0; i < zh_tbl_count(ZH_TBL_FIELDACC); i++) {
    const char* fn; uint16_t off; int base, size, wr;
    zh_tbl_fieldacc(i, &fn, &off, &base, &size, &wr);

    if (strcmp(fn, "Actor_getLevel") == 0) {
        LOG_I(LOG_HOOK, "Actor_getLevel: off=+0x%x base=x%d size=%d %s",
              off, base, size, wr ? "写" : "读");
        break;
    }
}
```

**输出**：
```
Actor_getLevel: off=+0x250 base=x0 size=8 读
```

### 2.2 语义含义

**如果函数名叫 `Actor_getLevel`，它的实现通常是**：

```asm
Actor_getLevel:
    ldr  x0, [x0, #0x250]     ← 这就是字段访问
    ret
```

**所以 `fieldacc` 表里 `base=0`（`x0` = this）、`off=0x250`、`size=8`。**

### 2.3 批量导出表

```c
static void dump_fields(const char* filter) {
    int n = zh_tbl_count(ZH_TBL_FIELDACC);
    LOG_I(LOG_HOOK, "=== 字段访问 %d 条 ===", n);

    for (int i = 0; i < n; i++) {
        const char* fn; uint16_t off; int base, size, wr;
        if (zh_tbl_fieldacc(i, &fn, &off, &base, &size, &wr) != 0) continue;
        if (filter && fn && strstr(fn, filter) == NULL) continue;

        LOG_I(LOG_HOOK, "  %-40s +0x%-5x base=x%-2d size=%d %s",
              fn ? fn : "?", off, base, size, wr ? "写" : "读");
    }
}
```

**调用**：

```c
dump_fields("Actor_");    // 只看 Actor 相关
dump_fields(NULL);        // 全部（716 条）
```

---

## 三、常见字段（Actor 类）

**从数据表可知**：

| 函数名 | 偏移 | 基址 | 宽度 | 含义 |
|---|---|---|---|---|
| `Actor_getLevel` | `+0x250` | x0 | 8 | `Level*` |
| `Actor_getVehicle` | `+0x10` | x0 | 8 | 载具指针 |
| `Actor_getRideRoot` | `+0x618` | x0 | 8 | 骑乘根 |
| `Actor_getDimensionBlockSource` | `+0x248` | x0 | 8 | BlockSource* |
| `Actor_addEffect` | `+0x8` | x0 | 1 | （写入） |
| `Actor_init` | `+0x70` | x0 | 8 | （写入） |

**完整 716 条**见 `data/fields_real.h`。

---

## 四、读取指针链

**很多字段是嵌套的**：

```c
// 例：从 Actor 拿 Level，再从 Level 拿某东西
void* level = ZH_THIS_PTR(c, 0x250);
if (level) {
    // 但 zh_field_get 只能从 c->x[argIdx] 读
    // 要读 level 里的字段，需要直接内存访问
    int32_t something = *(int32_t*)((char*)level + 0x100);
}
```

**直接内存访问是合法的** —— 只要地址有效。

**更安全的写法**：

```c
static int safe_read_i32(void* base, uint16_t off, int32_t* out) {
    if (!base) return -1;
    // 可选：验证 base 在合法范围
    if ((uintptr_t)base < 0x10000) return -1;
    *out = *(volatile int32_t*)((char*)base + off);
    return 0;
}
```

---

## 五、写入字段

### 5.1 直接写

```c
static void on_tick(ii_ctx* c, void* user) {
    (void)user;

    // 把某字段改成固定值
    zh_field_set(c, 0, 0x2a0, 4, 100);   // int 字段设为 100
}
```

### 5.2 读改写

```c
int64_t v = zh_field_get(c, 0, 0x2a0, 4);
zh_field_set(c, 0, 0x2a0, 4, v * 2);
```

### 5.3 注意事项

| 事项 | 说明 |
|---|---|
| 类型要对 | `size` 必须与字段实际宽度一致 |
| 对齐 | ARM64 非对齐访问可能失败（虽 Cortex 支持，但慢） |
| 写时机 | 在合适的 hook 点写，太早会被覆盖 |
| 线程安全 | 游戏可能在别的线程读写同一字段 |

---

## 六、完整示例

```c
#include <string.h>
#include <unistd.h>
#include "zh/zh_layer.h"
#include "zh/zlog.h"

// ═══ 查字段偏移 ═══
static int find_field(const char* func, uint16_t* off, int* base, int* size, int* wr) {
    for (int i = 0; i < zh_tbl_count(ZH_TBL_FIELDACC); i++) {
        const char* fn;
        if (zh_tbl_fieldacc(i, &fn, off, base, size, wr) != 0) continue;
        if (fn && strcmp(fn, func) == 0) return 1;
    }
    return 0;
}

// ═══ 回调：读字段 ═══
static volatile unsigned long g_reads = 0;
static uint16_t g_level_off = 0;
static int      g_level_size = 0;

static void on_tick(ii_ctx* c, void* user) {
    (void)user;
    if (!g_level_off) return;

    void* level = (void*)(uintptr_t)zh_field_get(c, 0, g_level_off, g_level_size);
    if (level) {
        __atomic_add_fetch(&g_reads, 1, __ATOMIC_RELAXED);
    }
}

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;

    // 1. 查字段
    uint16_t off; int base, size, wr;
    if (find_field("Actor_getLevel", &off, &base, &size, &wr)) {
        g_level_off = off;
        g_level_size = size;
        LOG_I(LOG_HOOK, "Actor_getLevel 字段: +0x%x base=x%d size=%d",
              off, base, size);
    } else {
        LOG_E(LOG_HOOK, "找不到字段");
        return NULL;
    }

    // 2. 装 hook
    zh_hook("LocalPlayer_normalTick", on_tick, NULL);

    // 3. 观察
    while (1) {
        sleep(5);
        LOG_I(LOG_HOOK, "读到非空 level %lu 次",
              __atomic_load_n(&g_reads, __ATOMIC_RELAXED));
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

## 七、与 Mixin `@Accessor` 对比

| Mixin | Nexus |
|---|---|
| `@Accessor("health")` | `zh_field_get(c, 0, off, size)` |
| 用字段名 | 用偏移 |
| 编译期解析 | 查数据表 |
| 版本更新自动适配 | 需重采集 |
| 类型由方法签名决定 | 手工指定 size |

**Mixin**：
```java
@Mixin(PlayerEntity.class)
public interface PlayerAccessor {
    @Accessor("health")
    float getHealth();
}
```

**Nexus**：
```c
// 先查偏移
uint16_t off; int base, size, wr;
find_field("Player_getHealth", &off, &base, &size, &wr);

// 再读写
float hp = (float)zh_field_get(c, 0, off, size);
```

---

## 八、陷阱

### 8.1 基址寄存器搞错

**`base=0` 表示 `x0`（this），`base=1` 表示 `x1`（第一个参数）。**

```c
// 如果数据表说 base=1，你传 argIdx=0，读的就是错的
zh_field_get(c, 1, off, size);   // 正确
zh_field_get(c, 0, off, size);   // 错误
```

### 8.2 宽度搞错

```c
zh_field_get(c, 0, 0x2a0, 8);   // 读 8 字节
zh_field_get(c, 0, 0x2a0, 4);   // 读 4 字节
```

**宽度不对会读到相邻字段的高位/低位。**

### 8.3 偏移已失效（版本更新）

**症状**：读到的值看起来是垃圾（如 `0xdeadbeef` 或极大数）。

**诊断**：

```c
int64_t v = zh_field_get(c, 0, off, 8);
// 检查是否是合法指针
if (v != 0 && (v < 0x10000 || v > 0x800000000000ULL)) {
    LOG_W(LOG_HOOK, "字段值可疑: 0x%llx（偏移可能已失效）", v);
}
```

---

## 九、性能

**`zh_field_get` 只有几层间接**：

```c
int64_t zh_field_get(ii_ctx* c, uint8_t argIdx, uint16_t off, uint8_t size) {
    uint64_t base = c->x[argIdx];
    if (!base) return 0;

    switch (size) {
        case 1: return *(int8_t*)(base + off);
        case 2: return *(int16_t*)(base + off);
        case 4: return *(int32_t*)(base + off);
        case 8: return *(int64_t*)(base + off);
        default: return 0;
    }
}
```

**实测**：3000 次/秒无压力（t06）。

---

## 下一步

- [栈局部变量](27-API-Stack-Local)
- [常量修改](25-API-Constant-Modify)
- [数据表查询](31-API-Data-Table)
