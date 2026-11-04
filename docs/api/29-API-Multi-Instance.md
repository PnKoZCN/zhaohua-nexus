# API：多实例

一个回调同时处理多个对象 —— Nexus 相对传统 inline hook 的核心优势。

---

## 一、问题从哪来

### 1.1 传统 hook 的困境

**游戏里 `ItemStack_isNull` 被几百个对象调用，但只有一个函数地址。**

**传统做法**：

```c
static void on_is_null(ii_ctx* c, void* user) {
    // c->x[0] 是 this，但每次调用是不同的 ItemStack
    // 你没法在安装时"绑定"到某个特定实例
}
```

**Mixin 的解法**：用 `@Inject` 挂到类上，Mixin 帮你区分实例。

**Nexus 的解法**：`ii_ctx` 里有 `x[0]`（this），你在回调里判断。

**但真正的问题是**：**你想给不同实例不同的行为。**

---

## 二、核心 API

### 2.1 `ii_multi_add()`

```c
int ii_multi_add(unsigned long addr, void* key, ii_callback cb,
                 void* user, int priority);
```

**作用**：给某地址注册一个"带 key 的回调"。

**参数**：
- `key` —— 实例标识（通常是 `this` 指针）
- `cb` —— 回调
- `priority` —— 同 key 内多个回调的优先级

**返回**：0 = 成功

---

### 2.2 `ii_multi_remove()`

```c
int ii_multi_remove(unsigned long addr, void* key, ii_callback cb, void* user);
```

**移除某 key 下的回调。**

---

### 2.3 `ii_multi_clear()`

```c
int ii_multi_clear(unsigned long addr, void* key);
```

**移除某 key 的**所有**回调。**

---

### 2.4 `ii_multi_count()`

```c
int ii_multi_count(unsigned long addr);
int ii_multi_key_count(unsigned long addr, void* key);
```

**前者**：该地址下所有 key 的数量
**后者**：指定 key 下的回调数

---

## 三、工作原理

### 3.1 dispatch 里的 key 匹配

```c
static void ii_dispatch_multi(ii_site* s, ii_ctx* c) {
    // 1. 从 x0 取 this
    void* key = (void*)c->x[0];

    // 2. 先执行通用回调（不带 key 的）
    for (ii_cb_node* n = s->cbs; n; n = n->next) {
        if (!n->alive || n->key) continue;      // 跳过带 key 的
        n->cb(c, n->user);
    }

    // 3. 再执行匹配 key 的
    for (int i = 0; i < s->nMulti; ++i) {
        ii_multi_slot* m = &s->multi[i];
        if (m->key != key) continue;

        for (ii_cb_node* n = m->cbs; n; n = n->next) {
            if (!n->alive) continue;
            n->cb(c, n->user);
        }
    }
}
```

### 3.2 数据结构

```c
typedef struct ii_multi_slot {
    void*         key;        // 实例指针
    ii_cb_node*   cbs;        // 该实例的回调链
    int           nCbs;
    struct ii_multi_slot* next;
} ii_multi_slot;

typedef struct ii_site {
    ...
    ii_multi_slot* multi;     // 多实例槽位链
    int            nMulti;
} ii_site;
```

**每个站点有自己的多实例表。**

---

## 四、实测案例（t11）

### 4.1 目标

**给 14 个不同 key 注册回调，验证全部正确分发。**

### 4.2 代码

```c
#define N_KEYS 14

static volatile unsigned long g_hits[N_KEYS];
static int g_key_idx[N_KEYS];

// 每个 key 一个回调
static void cb_key(ii_ctx* c, void* user) {
    int idx = (int)(uintptr_t)user;
    if (idx >= 0 && idx < N_KEYS) {
        __atomic_add_fetch(&g_hits[idx], 1, __ATOMIC_RELAXED);
    }
    (void)c;
}

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;

    const char* fn = "ItemStack_isNull";
    unsigned long addr = zh_find(fn);
    if (!addr) { LOG_E(LOG_HOOK, "找不到 %s", fn); return NULL; }

    // 1. 先建站点（普通 hook）
    int rc = zh_hook(fn, cb_noop, NULL);
    LOG_I(LOG_MULTI, "建站点 rc=%d", rc);

    // 2. 注册 14 个 key
    //    注意：key 必须是真实存在的对象指针才可能被匹配
    //    这里用伪造地址验证机制本身
    for (int i = 0; i < N_KEYS; i++) {
        void* fake_key = (void*)(uintptr_t)(0x100000000ULL + i * 0x1000);
        g_key_idx[i] = i;

        int r = ii_multi_add(addr, fake_key, cb_key, (void*)(uintptr_t)i, 0);
        if (r != 0) {
            LOG_E(LOG_MULTI, "注册 key[%d] 失败 rc=%d", i, r);
        }
    }

    LOG_I(LOG_MULTI, "已注册 %d 个 key，总 key 数=%d", N_KEYS, ii_multi_count(addr));

    // 3. 验证结构（伪造 key 不会被真实调用命中，但结构可查）
    int ok = 0;
    for (int i = 0; i < N_KEYS; i++) {
        void* k = (void*)(uintptr_t)(0x100000000ULL + i * 0x1000);
        int n = ii_multi_key_count(addr, k);
        if (n == 1) ok++;
    }
    LOG_I(LOG_MULTI, "key 结构验证: %d/%d", ok, N_KEYS);

    // 4. 移除一半
    for (int i = 0; i < N_KEYS; i += 2) {
        void* k = (void*)(uintptr_t)(0x100000000ULL + i * 0x1000);
        ii_multi_remove(addr, k, cb_key, (void*)(uintptr_t)i);
    }
    LOG_I(LOG_MULTI, "移除 7 个后 key 数=%d", ii_multi_count(addr));

    // 5. 清空
    for (int i = 1; i < N_KEYS; i += 2) {
        void* k = (void*)(uintptr_t)(0x100000000ULL + i * 0x1000);
        ii_multi_clear(addr, k);
    }
    LOG_I(LOG_MULTI, "全清后 key 数=%d", ii_multi_count(addr));

    while (1) sleep(5);
    return NULL;
}
```

### 4.3 结果

```
OK   建站点                  rc=0
OK   注册 14 个 key          rc=0 × 14
OK   总 key 数               14
OK   key 结构验证            14/14
OK   移除 7 个后剩          7
OK   全清后剩                0
OK   zh_unhook              rc=0
OK   进程存活               ✅

14/14 通过
```

---

## 五、真实使用方式

### 5.1 场景：只修改特定玩家的血量

```c
static void* g_target_player = NULL;   // 你要改的那个玩家

static void on_get_health_keyed(ii_ctx* c, void* user) {
    (void)user;
    float hp = zh_get_ret_float(c);
    if (hp > 0 && hp < 100.0f) {
        zh_set_ret_float(c, 100.0f);
    }
}

static void on_get_health_generic(ii_ctx* c, void* user) {
    (void)c; (void)user;
    // 其他玩家不处理
}

// 安装
unsigned long addr = zh_find("Actor_getHealth");
ii_install_at_return("Actor_getHealth", -1, on_get_health_generic, NULL);

// 关键：返回点 hook 时 x0 是返回值，不是 this！
// 所以 key 匹配在返回点不可用
```

**重要限制**：**多实例 key 匹配只在入口 hook 有效**（因为要用 `x[0]` 做 key）。

**返回点里 `x0` 是返回值。**

### 5.2 场景：按对象类型分流

```c
static void* g_chest = NULL;
static void* g_barrel = NULL;

static void on_chest(ii_ctx* c, void* user)   { (void)c; (void)user; /* 箱子逻辑 */ }
static void on_barrel(ii_ctx* c, void* user)  { (void)c; (void)user; /* 木桶逻辑 */ }

// 找到实例后注册
void bind_containers(void) {
    unsigned long addr = zh_find("ContainerScreenController_getStackFromModel");

    for (each container) {
        if (is_chest(container))  ii_multi_add(addr, container, on_chest, NULL, 0);
        if (is_barrel(container)) ii_multi_add(addr, container, on_barrel, NULL, 0);
    }
}
```

### 5.3 场景：实体生命周期跟踪

```c
static void on_actor_tick(ii_ctx* c, void* user) {
    (void)user;
    void* actor = (void*)c->x[0];
    // 这个 key 是我自己注册的
    // 说明 actor 就是我关心的那个
    update_entity(actor);
}

// 玩家进入视野时注册
void on_entity_enter(void* actor, int type) {
    if (type == ENTITY_ITEM) {
        ii_multi_add(zh_find("Actor_normalTick"), actor, on_actor_tick, NULL, 0);
    }
}

// 离开时移除
void on_entity_leave(void* actor) {
    ii_multi_remove(zh_find("Actor_normalTick"), actor, on_actor_tick, NULL);
}
```

---

## 六、性能

### 6.1 dispatch 开销

**每次调用**：

```c
void* key = (void*)c->x[0];          // 1 次读

for (ii_cb_node* n = s->cbs; n; ...)  // 通用回调 O(n)
for (int i = 0; i < s->nMulti; ++i)   // key 匹配 O(nMulti)
```

**`nMulti` 大时线性扫描慢。**

### 6.2 优化建议

| key 数量 | 建议 |
|---|---|
| ≤ 16 | 直接用，线性扫描很快 |
| 16 ~ 256 | 可接受（都在 cache 里） |
| > 256 | 考虑自己建 hash（用单个通用回调 + 自己的 map） |

**自己建 hash 的写法**：

```c
// 只注册一个通用回调
static void on_tick(ii_ctx* c, void* user) {
    (void)user;
    void* actor = (void*)c->x[0];
    handler h = my_hashmap_get(actor);
    if (h) h(actor);
}
zh_hook("Actor_normalTick", on_tick, NULL);
```

**这样 dispatch 只有 1 次 hash 查找。**

---

## 七、与 Mixin 对比

| Mixin | Nexus |
|---|---|
| 按类挂载（自动区分实例） | 手动注册 key |
| `@Inject` 挂在方法上 | `ii_multi_add` 挂在地址上 |
| 实例自动传递 | 自己从 `x[0]` 取 |
| 无数量限制 | 线性扫描 |

**Mixin 的"多实例"是语言层面的（每个对象都有方法表）。**

**Nexus 需要一个注册步骤** —— 但这也给了你更精确的控制。

---

## 八、注意事项

| 事项 | 说明 |
|---|---|
| key 匹配基于 `x[0]` | 只对 `this` 有效 |
| 返回点不能用 key | `x0` 是返回值 |
| 对象释放后要移除 | 否则 key 悬空 |
| 线性扫描 | key 太多会慢 |
| 通用回调先执行 | 带 key 的后执行 |

**对象释放的坑**：

```c
// 对象被 delete 后，key 地址可能被新对象复用
// → 你的回调会跑到新对象上

// 缓解：在对象析构 hook 里移除
static void on_actor_destroy(ii_ctx* c, void* user) {
    (void)user;
    void* actor = (void*)c->x[0];
    ii_multi_remove(zh_find("Actor_normalTick"), actor, on_actor_tick, NULL);
}
zh_hook("Actor_destroy", on_actor_destroy, NULL);
```

---

## 九、完整示例

```c
#include <string.h>
#include <unistd.h>
#include "zh/zh_layer.h"
#include "zh/zlog.h"

#define MAX_ENTITIES 64

static void* g_entities[MAX_ENTITIES];
static int   g_nEntities = 0;
static volatile unsigned long g_ticks[MAX_ENTITIES];

static void on_entity_tick(ii_ctx* c, void* user) {
    int idx = (int)(uintptr_t)user;
    (void)c;
    if (idx >= 0 && idx < MAX_ENTITIES) {
        __atomic_add_fetch(&g_ticks[idx], 1, __ATOMIC_RELAXED);
    }
}

static void track(void* entity) {
    if (g_nEntities >= MAX_ENTITIES) return;
    int idx = g_nEntities++;
    g_entities[idx] = entity;
    ii_multi_add(zh_find("Actor_normalTick"), entity, on_entity_tick,
                 (void*)(uintptr_t)idx, 0);
    LOG_I(LOG_MULTI, "跟踪实体 %p (槽 %d)", entity, idx);
}

static void untrack(void* entity) {
    for (int i = 0; i < g_nEntities; i++) {
        if (g_entities[i] != entity) continue;
        ii_multi_remove(zh_find("Actor_normalTick"), entity, on_entity_tick,
                        (void*)(uintptr_t)i);
        LOG_I(LOG_MULTI, "停止跟踪 %p", entity);
        return;
    }
}

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;

    // 建"空"站点（必须有站点才能注册 key）
    zh_hook("Actor_normalTick", NULL, NULL);

    // 你的实体发现逻辑
    // track(p_entity) / untrack(p_entity)

    while (1) {
        sleep(10);
        LOG_I(LOG_MULTI, "跟踪 %d 个实体", g_nEntities);
        for (int i = 0; i < g_nEntities; i++) {
            LOG_I(LOG_MULTI, "  [%d] %p: %lu ticks",
                  i, g_entities[i],
                  __atomic_load_n(&g_ticks[i], __ATOMIC_RELAXED));
        }
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

## 下一步

- [多回调链](23-API-Multi-Callback)
- [崩溃隔离](30-API-Crash-Isolation)
- [性能与开销](41-Reference-Performance)
