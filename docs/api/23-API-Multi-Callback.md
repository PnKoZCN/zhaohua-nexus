# API：多回调链

同一地址挂多个回调，按优先级执行 —— 对标 Mixin 多个 `@Inject`。

---

## 一、核心 API

### 1.1 `ii_add_callback()`

```c
int ii_add_callback(unsigned long addr, ii_callback cb, void* user, int priority);
```

**作用**：向已存在的站点追加回调。

**参数**：
- `addr` —— 站点地址（必须是已安装的）
- `priority` —— 优先级，**大的先执行**

**返回**：0 = 成功，-2 = 站点不存在，-9 = 内存不足

**示例**：

```c
// 先建站点
zh_hook("func", cb_base, NULL);              // priority 0

// 追加
ii_add_callback(addr, cb_high, NULL, 300);   // 先执行
ii_add_callback(addr, cb_mid,  NULL, 100);   // 后执行

// 执行顺序：cb_high(300) → cb_mid(100) → cb_base(0)
```

---

### 1.2 `ii_remove_callback()`

```c
int ii_remove_callback(unsigned long addr, ii_callback cb, void* user);
```

**作用**：从链上移除一个回调。

**注意**：这是**懒删除**（`alive = 0`），节点不立刻释放。

**站点的其他回调继续工作。**

---

### 1.3 `ii_callback_count()`

```c
int ii_callback_count(unsigned long addr);
```

**返回**：链上**活跃**回调数量。

```c
int n = ii_callback_count(addr);
LOG_I(LOG_HOOK, "链上有 %d 个回调", n);
```

---

### 1.4 `ii_set_site_priority()` / `ii_site_priority()`

```c
int ii_set_site_priority(unsigned long addr, int priority);
int ii_site_priority(unsigned long addr);
```

**作用**：调整整个站点的优先级（影响 `zhreg` 共享区的排序）。

---

## 二、执行顺序

### 2.1 按 priority 降序

```c
ii_add_callback(addr, cb_a, NULL, 0);
ii_add_callback(addr, cb_b, NULL, 300);
ii_add_callback(addr, cb_c, NULL, 100);

// 执行：b(300) → c(100) → a(0)
```

**实测验证**（t08）：

```
安装：zh_hook(cb_mid, 默认 0) + ii_add_callback(cb_hi, 300) + ii_add_callback(cb_lo, 100)
执行顺序：300 → 100 → 0   ✅
```

### 2.2 相同 priority

**按插入顺序**（后插的排在后面）：

```c
ii_add_callback(addr, cb_1, NULL, 100);
ii_add_callback(addr, cb_2, NULL, 100);

// 执行：cb_1 → cb_2
```

### 2.3 插入排序实现

```c
static int chain_insert(ii_site* s, ii_callback cb, void* user, int prio) {
    ii_cb_node* n = cbnode_new(cb, user, prio);
    n->owner = g_curOwner;

    if (!s->cbs || s->cbs->priority < prio) {
        n->next = s->cbs;
        s->cbs = n;                    // 插到头部
    } else {
        ii_cb_node* p = s->cbs;
        while (p->next && p->next->priority >= prio) p = p->next;
        n->next = p->next;
        p->next = n;
    }
    s->nCbs++;
    return 0;
}
```

**O(n) 插入，n 通常 ≤ 3。**

---

## 三、提前终止

### 3.1 用 `ii_cancel()` 打断

```c
static void cb_a(ii_ctx* c, void* user) {
    if (should_stop()) {
        ii_cancel(c);      // 后续回调和函数体都不会执行
    }
}

static void cb_b(ii_ctx* c, void* user) {
    // 如果 cb_a 调了 ii_cancel，这里不会执行
}
```

**`ii_dispatch` 里的检查**：

```c
for (ii_cb_node* n = s->cbs; n; n = n->next) {
    if (!n->alive || !n->cb) continue;
    n->cb(c, n->user);
    if (g_cancelReq || II_IS_CANCELLED(c)) break;   // ← 打断
}
```

### 3.2 限制

**`ii_cancel` 只在函数入口生效。**

在返回点 cancel 会破坏栈帧（callee-saved 寄存器没恢复）。

---

## 四、优先级设计建议

| 用途 | 建议 priority |
|---|---|
| 前置检查 / 拦截 | 1000+ |
| 数据修正 | 500~900 |
| 主要逻辑 | 100~400 |
| 观察 / 统计 | 0~50 |
| 兜底 / 日志 | 负数 |

**示例**：

```c
ii_add_callback(addr, cb_intercept, NULL, 1000);   // 最先，可能 cancel
ii_add_callback(addr, cb_fix_data,  NULL,  500);
ii_add_callback(addr, cb_main,      NULL,  100);
zh_hook(addr, cb_stats, NULL);                     // 0，最后统计
```

---

## 五、完整示例

```c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "zh/zh_layer.h"
#include "zh/zlog.h"

static volatile unsigned long g_order[8];
static volatile int g_n = 0;

static void cb_hi(ii_ctx* c, void* u) {
    (void)c; (void)u;
    int i = __atomic_fetch_add(&g_n, 1, __ATOMIC_RELAXED);
    if (i < 8) g_order[i] = 300;
}

static void cb_mid(ii_ctx* c, void* u) {
    (void)c; (void)u;
    int i = __atomic_fetch_add(&g_n, 1, __ATOMIC_RELAXED);
    if (i < 8) g_order[i] = 200;
}

static void cb_lo(ii_ctx* c, void* u) {
    (void)c; (void)u;
    int i = __atomic_fetch_add(&g_n, 1, __ATOMIC_RELAXED);
    if (i < 8) g_order[i] = 100;
}

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;

    const char* fn = "LocalPlayer_normalTick";
    unsigned long addr = zh_find(fn);
    if (!addr) return NULL;

    // 1. 建站点（priority 默认 0）
    int r1 = zh_hook(fn, cb_lo, NULL);
    LOG_I(LOG_HOOK, "建站点 rc=%d", r1);

    // 2. 追加两个
    int r2 = ii_add_callback(addr, cb_mid, NULL, 200);
    int r3 = ii_add_callback(addr, cb_hi,  NULL, 300);
    LOG_I(LOG_HOOK, "追加 rc=%d/%d, 链长=%d", r2, r3, ii_callback_count(addr));

    // 3. 观察
    __atomic_store_n(&g_n, 0, __ATOMIC_RELAXED);
    sleep(1);

    LOG_I(LOG_HOOK, "执行顺序: %ld %ld %ld",
          g_order[0], g_order[1], g_order[2]);
    // 期望：300 200 100

    // 4. 移除中间那个
    ii_remove_callback(addr, cb_mid, NULL);
    LOG_I(LOG_HOOK, "移除后链长=%d", ii_callback_count(addr));

    // 5. 全清
    zh_unhook(fn);
    LOG_I(LOG_HOOK, "卸载后链长=%d", ii_callback_count(addr));

    while (1) sleep(5);
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
```

**期望输出**：
```
建站点 rc=0
追加 rc=0/0, 链长=3
执行顺序: 300 200 100
移除后链长=2
卸载后链长=0
```

---

## 六、跨模块协作

### 6.1 不同 Mod 挂同一函数

```c
// Mod A
zh_hook("LocalPlayer_normalTick", cb_a, NULL);

// Mod B（同一个进程）
zh_hook("LocalPlayer_normalTick", cb_b, NULL);
// 返回 -2（已有站点），但...

// 正确做法
unsigned long addr = zh_find("LocalPlayer_normalTick");
if (zh_hook("LocalPlayer_normalTick", cb_b, NULL) == -2) {
    ii_add_callback(addr, cb_b, NULL, 0);   // 追加到已有链
}
```

**更简洁**：

```c
// 先试 zh_hook，失败就追加
int rc = zh_hook(fn, cb, NULL);
if (rc == -2) {
    ii_add_callback(zh_find(fn), cb, NULL, 0);
}
```

### 6.2 跨探针共享

**不同 `.so` 里的模块挂同一函数**：

- 第一个模块建立站点
- 第二个模块 `zh_hook` 返回 -2
- 第二个模块 `ii_add_callback` → 但这是**本地**链
- 真正跨 `.so` 是通过 `zhreg` 共享区

**`ii_install_ex` 里的处理**：

```c
if (g_reg) {
    int zidx = zhreg_find_site(g_reg, addr);
    if (zidx >= 0) {
        // 别的探针已插桩 → 挂到共享链
        zhreg_add_cb(g_reg, zidx, cb, user, g_myId, priority);
        return 0;
    }
}
```

**`ii_dispatch` 遍历两处**：

```c
// 1. 本地链
for (ii_cb_node* n = s->cbs; n; n = n->next) { ... }

// 2. 共享区的外部回调
if (s->regIdx >= 0 && g_reg) {
    if (zhreg_foreign_cb(g_reg, s->regIdx, g_myId) > 0) {
        for (int k = 0; k < zs->nCb; ++k) {
            if (!zs->cb[k].alive || zs->cb[k].owner == g_myId) continue;
            ((ii_callback)zs->cb[k].cb)(c, ...);
        }
    }
}
```

---

## 七、懒删除机制

### 7.1 为什么不立刻 free

**`ii_dispatch` 在热路径上遍历链**（每次函数调用都走）。

如果 `chain_remove` 直接 `free(n)`：

```
线程 A: 遍历到 n，调用 n->cb
线程 B: chain_remove → free(n)
线程 A: n = n->next  ← UAF！
```

**真机表现**：卸载后几秒内卡死闪退。

### 7.2 正确实现

```c
static int chain_remove(ii_site* s, ii_callback cb, void* user) {
    ii_cb_node** pp = &s->cbs;
    while (*pp) {
        ii_cb_node* n = *pp;
        if (n->cb == cb && n->user == user) {
            // ★ 懒删除：只置 alive = 0
            n->alive = 0;
            if (s->nCbs > 0) s->nCbs--;
            return 0;
        }
        pp = &n->next;
    }
    return -1;
}
```

**真正的 free 在 `chain_clear()`**（站点整体销毁时）。

### 7.3 遍历时跳过

```c
for (ii_cb_node* n = s->cbs; n; n = n->next) {
    if (!n->alive || !n->cb) continue;   // ← 跳过死节点
    n->cb(c, n->user);
}
```

---

## 八、慢回调告警

**每个节点记录是否告警过**：

```c
uint64_t t0 = zh_now_ms();
n->cb(c, n->user);
uint64_t dt = zh_now_ms() - t0;

if (dt >= II_CB_SLOW_MS) {    // 50 ms
    if (!n->slowWarned) {
        n->slowWarned = 1;
        wd_log("[SLOW] 回调 @0x%lx 耗时 %llu ms（阈值 %d）。"
               "回调运行在游戏线程上，重操作会冻结游戏，请改为异步。\n",
               s->addr, dt, II_CB_SLOW_MS);
    }
}
```

**只告警一次**（避免日志爆炸）。

---

## 九、注意事项

| 事项 | 说明 |
|---|---|
| 追加到不存在的站点 | 返回 -2，需先 `zh_hook` |
| LIGHT 站点不支持多回调 | 返回 -16 |
| 移除最后一个回调 | 站点保留（`installed` 仍为 1） |
| `ii_callback_count` | 只数 `alive` 节点 |
| 死节点会堆积 | 直到站点销毁时统一 free |

---

## 下一步

- [Hook 安装](22-API-Hook-Install)
- [多实例](29-API-Multi-Instance)
- [性能与开销](41-Reference-Performance)
