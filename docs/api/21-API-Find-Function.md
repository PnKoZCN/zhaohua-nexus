# API：函数查找

按名字查地址，是所有操作的第一步。

---

## 一、`zh_find()`

```c
unsigned long zh_find(const char* funcName);
```

**作用**：按函数名查绝对地址。

**返回**：0 = 找不到。

**示例**：

```c
unsigned long addr = zh_find("LocalPlayer_normalTick");
if (!addr) {
    LOG_E(LOG_HOOK, "函数不在数据表");
    return;
}
LOG_I(LOG_HOOK, "addr = 0x%lx (base+0x%lx)", addr, addr - zh_module_base());
```

**数据表里有 124 个函数**（见 `data/ret_db.h`），完整列表可这样枚举：

```c
for (int i = 0; i < zh_tbl_count(ZH_TBL_RETFUNC); i++) {
    const char* name = zh_tbl_retfunc(i);
    if (name) LOG_I(LOG_HOOK, "  %s", name);
}
```

---

## 二、`zh_find_verbose()`

```c
unsigned long zh_find_verbose(const char* funcName);
```

**作用**：同 `zh_find`，但失败时会打日志说明原因。

**区别**：

```c
// zh_find：静默失败
unsigned long a = zh_find("不存在的函数");   // 返回 0，无日志

// zh_find_verbose：会说明为什么失败
unsigned long b = zh_find_verbose("不存在的函数");
// 日志：函数不在数据表: 不存在的函数
```

**推荐**：调试期用 `_verbose`，生产用 `zh_find`。

---

## 三、`zh_find_h()`

```c
unsigned long zh_find_h(const char* funcName, int hint);
```

**作用**：带提示的查找（用于加速或消歧）。

**`hint` 参数**：给数据库一个查找起点提示（当前实现主要为将来预留）。

**目前建议直接用 `zh_find()`。**

---

## 四、地址转换

### 4.1 相对偏移 ↔ 绝对地址

```c
unsigned long base = zh_module_base();

// 数据表里存的是相对偏移
unsigned long rel = 0x759b6c8;

// 转绝对
unsigned long abs_addr = base + rel;

// 反推
unsigned long back = abs_addr - base;
```

### 4.2 从数据表查函数的相对偏移

```c
// 遍历 rettype 表，找函数名对应的偏移
for (int i = 0; i < zh_tbl_count(ZH_TBL_RETTYPE); i++) {
    const char* name; uint32_t off; int kind; const char* tn;
    zh_tbl_rettype(i, &name, &off, &kind, &tn);
    if (name && strcmp(name, "Actor_getPos") == 0) {
        LOG_I(LOG_HOOK, "Actor_getPos 偏移 = 0x%x", off);
        LOG_I(LOG_HOOK, "返回类型 = %s (kind=%d)", tn, kind);
        break;
    }
}
```

---

## 五、验证函数存在性

```c
static int func_exists(const char* name) {
    unsigned long a = zh_find(name);
    if (!a) return 0;

    // 进一步验证：读第一条指令，检查是否有效
    uint32_t w = *(volatile uint32_t*)a;
    if (w == 0 || w == 0xFFFFFFFFu) return 0;

    return 1;
}
```

---

## 六、常见函数名

### 6.1 高频 tick 类（适合做定时逻辑）

| 函数名 | 频率 | 说明 |
|---|---|---|
| `LocalPlayer_normalTick` | 20 TPS | 本地玩家 tick |
| `Level_tick` | 20 TPS | 世界 tick |
| `Player_tickWorld` | 20 TPS | 玩家世界 tick |

### 6.2 常用 getter

| 函数名 | 返回 | 说明 |
|---|---|---|
| `Actor_getPos` | `Vec3*` | 实体坐标 |
| `Actor_getLevel` | `Level*` | 所在世界 |
| `Actor_getHealth` | `float` | 血量 |
| `Actor_getRideRoot` | `Actor*` | 骑乘根 |
| `Actor_getVehicle` | `Actor*` | 载具 |
| `ItemStack_isNull` | `bool` | 物品是否为空 |
| `Item_getRawNameId` | `uint16` | 物品 ID |

### 6.3 其他常用

| 函数名 | 说明 |
|---|---|
| `ContainerScreenController_getStackFromModel` | 容器槽位物品 |
| `ChestScreenController_handleAutoPlace` | 自动放置 |
| `Player_setPlayerGameType` | 设置游戏模式 |
| `GuiData_setActionBar` | 设置动作栏文字 |
| `MobEffect_getById` | 按 ID 查效果 |

**完整列表**：

```bash
# 从 data/ret_db.h 看
grep -oE '"[A-Za-z_0-9]+"' data/ret_db.h | sort -u | head -50
```

---

## 七、找不到函数怎么办

### 7.1 排查顺序

| 步骤 | 检查 |
|---|---|
| 1 | 游戏版本是否匹配 `3.10.5.301327` |
| 2 | `libminecraftpe.so` 大小是否为 323,630,848 |
| 3 | 函数名拼写（区分大小写） |
| 4 | 是否在数据表里（用 `zh_tbl_retfunc` 枚举） |

### 7.2 枚举所有可用函数

```c
LOG_I(LOG_HOOK, "数据表里有 %d 个函数：", zh_tbl_count(ZH_TBL_RETFUNC));
for (int i = 0; i < zh_tbl_count(ZH_TBL_RETFUNC); i++) {
    const char* n = zh_tbl_retfunc(i);
    if (n) LOG_I(LOG_HOOK, "  [%d] %s", i, n);
}
```

### 7.3 数据表里没有，但你知道地址

**直接用地址 hook**：

```c
unsigned long addr = zh_module_base() + 0x759b6c8;
int rc = ii_install(addr, cb, NULL);
```

**但**：这个地址是你自己算的，版本更新会失效。

---

## 八、`zh_name_to_off()`

```c
static uint32_t zh_name_to_off(const char* funcName);
```

**内部函数**（不导出），把函数名转相对偏移。

**外部不需要调用** —— `zh_find()` 已经封装了。

---

## 九、完整示例

```c
#include <string.h>
#include "zh/zh_layer.h"
#include "zh/zh_tbl.h"
#include "zh/zlog.h"

static void dump_functions(void) {
    unsigned long base = zh_module_base();
    int n = zh_tbl_count(ZH_TBL_RETFUNC);

    LOG_I(LOG_HOOK, "=== 可用函数 %d 个 (base=0x%lx) ===", n, base);

    for (int i = 0; i < n; i++) {
        const char* name = zh_tbl_retfunc(i);
        if (!name) continue;

        unsigned long addr = zh_find(name);
        if (!addr) continue;

        // 查返回类型
        int kind = ii_ret_kind(addr);
        const char* tn = ii_ret_type_name(addr);

        LOG_I(LOG_HOOK, "  %-42s +0x%-8lx  %s",
              name, addr - base, tn ? tn : "?");
    }
}

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;

    dump_functions();

    // 找一个高频函数做 tick
    const char* targets[] = {
        "LocalPlayer_normalTick",
        "Level_tick",
        "Player_tickWorld",
        NULL
    };

    for (int i = 0; targets[i]; i++) {
        if (zh_find(targets[i])) {
            LOG_I(LOG_HOOK, "选用 %s", targets[i]);
            zh_hook(targets[i], on_tick, NULL);
            break;
        }
    }

    while (1) sleep(5);
    return NULL;
}
```

---

## 下一步

- [Hook 安装](22-API-Hook-Install)
- [多回调链](23-API-Multi-Callback)
- [数据表查询](31-API-Data-Table)
