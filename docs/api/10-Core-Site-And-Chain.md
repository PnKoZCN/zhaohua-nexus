# 核心概念：站点与回调链

理解这两个概念，就理解了 Nexus 的骨架。

---

## 一、站点（Site）

### 1.1 什么是站点

**站点 = 一个被插桩的内存地址 + 它的插桩状态**

```c
typedef struct {
    unsigned long addr;         // 被插桩的指令地址
    unsigned int  orig;         // 原始指令（4 字节）
    unsigned long trampoline;   // 跳板（执行原指令后跳回）
    unsigned long shell;        // 薄壳（跳到 handler）
    int           installed;    // 是否已安装
    int           wideMode;     // 16 字节模式
    ii_cb_node*   cbs;          // 回调链
    int           nCbs;         // 链长
    int           regIdx;       // 跨探针注册表索引
    ...
} ii_site;
```

**关键点**：
- 一个地址只能有**一个**站点
- 一个站点可以有**多个**回调（链表）

### 1.2 站点在表里的位置

所有站点存在一张**全局表**里：

```c
static struct ii_engine g_defaultEngine;   // 全局引擎实例
#define g_siteChunk  (ii_default()->siteChunk)
#define g_nSites     (ii_default()->nSites)
```

**分段数组**（每段 256 个），永不搬移 —— 这样 `site_at(idx)` 拿到的指针永远有效。

```c
static inline ii_site* site_at(int idx) {
    if (idx < 0 || idx >= g_nSites) return NULL;
    int c = idx / II_SITE_CHUNK;
    ii_site** blk = g_siteChunk[c];
    return blk ? blk[idx % II_SITE_CHUNK] : NULL;
}
```

### 1.3 站点号（sid）

**每个站点有一个索引 `sid`，写进 shell 指令里**：

```asm
shell:
  stp  x16, x17, [sp, #-16]!
  movz x17, #sid          ← 站点号编码在指令里
  ldr  x16, #8
  br   x16
```

handler 用 `x17` 调用 `ii_dispatch(ctx, sid)`，后者 `site_at(sid)` 找回站点。

**限制**：`movz` 只能装 16 位立即数 → 最多 65,535 个站点。实际能力 262,144（`II_MAX_SITES`），但 `sid` 超过 65535 会截断。

---

## 二、回调链

### 2.1 为什么需要链

对标 Mixin 的多个 `@Inject`：

```java
@Mixin(PlayerEntity.class) class A {
    @Inject(method="tick", at=HEAD) void a(CallbackInfo ci) {}
}
@Mixin(PlayerEntity.class) class B {
    @Inject(method="tick", at=HEAD) void b(CallbackInfo ci) {}
}
// 两个回调都应该执行
```

Nexus 里：

```c
zh_hook("LocalPlayer_normalTick", cb_a, NULL);           // 建站点 + 挂 a
ii_add_callback(addr, cb_b, NULL, 0);                    // 追加 b
// 两个都会执行
```

### 2.2 节点结构

```c
typedef struct ii_cb_node {
    struct ii_cb_node* next;
    ii_callback        cb;          // 回调函数
    void*              user;        // 用户数据
    int                priority;    // 越大越先执行
    int                alive;       // 0 = 已删除（懒删除）
    int                owner;       // 归属实例 id
    int                slowWarned;  // 已告警过"太慢"
} ii_cb_node;
```

### 2.3 排序规则

**按 `priority` 降序**（大的先跑）：

```c
ii_add_callback(addr, cb_high, NULL, 300);
ii_add_callback(addr, cb_mid,  NULL, 100);
zh_hook(addr, cb_low, NULL);                   // 默认 priority = 0

// 执行顺序：cb_high(300) → cb_mid(100) → cb_low(0)
```

**Mixin 的 `priority` 语义相同**（数字大的先执行）。

### 2.4 懒删除

**关键设计**：删除回调时**不立刻 free**，只置 `alive = 0`。

```c
static int chain_remove(ii_site* s, ii_callback cb, void* user) {
    ...
    // ★ 懒删除：只把 alive 清 0，不从链上摘、也不 free
    n->alive = 0;
    if (s->nCbs > 0) s->nCbs--;
    return 0;
}
```

**为什么**：

`ii_dispatch` 在**热路径**上遍历这条链（每次被 hook 的函数都走一遍）。如果 `chain_remove` 直接 `*pp = n->next; free(n)`，正在遍历的线程下一轮读 `n->next` 就是 **use-after-free**。

真机表现：**卸载后几秒内卡死闪退**（因为被 hook 的是每帧调用的函数，几乎必然撞上）。

**真正的 free 在 `chain_clear()`** —— 只在站点整体销毁时调用（那时入口指令已恢复，不会再有线程跳进来）。

### 2.5 遍历时跳过死节点

```c
if (s->cbs) {
    for (ii_cb_node* n = s->cbs; n; n = n->next) {
        if (!n->alive || !n->cb) continue;    // ← 跳过已删除的
        n->cb(c, n->user);
        if (g_cancelReq || II_IS_CANCELLED(c)) break;
    }
}
```

---

## 三、两者关系

```
              函数入口 0x7c4cfba6c8
                     │
                     │ (4 字节原子写: b shell)
                     ▼
               ┌─────────────┐
               │   站点      │  ← ii_site，全局表 idx = sid
               │  addr       │
               │  orig       │
               │  shell      │
               │  trampoline │
               │  cbs ───────┼──→ [node: priority=300, cb=cb_high]
               │  nCbs = 2   │    [node: priority=100, cb=cb_low]
               └─────────────┘
```

**一个站点 = 一处插桩 = 一条回调链**

---

## 四、生命周期

### 4.1 安装

```c
zh_hook("func", cb, user)
  │
  ├─ 1. zh_find("func") → 拿地址
  ├─ 2. ii_find(addr) → 已有站点？
  │     ├─ 有 → chain_insert（追加回调），返回
  │     └─ 无 → 继续
  ├─ 3. 分配 shell + trampoline（alloc_shell_near）
  ├─ 4. 写 shell 指令
  ├─ 5. 生成 trampoline
  ├─ 6. patch 原指令为 `b shell`
  └─ 7. 登记到站点表 + 回调链
```

### 4.2 卸载

```c
zh_unhook("func")
  │
  ├─ 1. ii_find(addr) → 找站点
  ├─ 2. site_disable
  │     ├─ 写回原始指令
  │     └─ 校验写回成功
  ├─ 3. site_release
  │     ├─ 归还 shell 槽位（进隔离区，3 秒后才可复用）
  │     ├─ 从站点表摘除
  │     ├─ chain_clear（释放所有节点）
  │     └─ site_retire（延迟 3 秒 free 站点结构）
  └─ 完成
```

**关键：两处延迟**

1. **shell 槽位**进隔离区 3 秒 —— 防止正在执行的线程跑进别人的 shell
2. **站点结构**进退休队列 3 秒 —— 防止 `ii_dispatch` 读到已释放内存

```c
#define ZHREG_QUARANTINE_MS 3000
#define II_RETIRE_DELAY_MS  3000
```

### 4.3 追加回调

```c
ii_add_callback(addr, cb2, user2, 200)
  │
  ├─ 1. ii_find(addr) → 找站点
  ├─ 2. chain_insert(s, cb2, user2, 200)  → 按 priority 插入
  └─ 3. nCbs++
```

### 4.4 移除单个回调

```c
ii_remove_callback(addr, cb, user)
  │
  ├─ 1. ii_find(addr) → 找站点
  ├─ 2. chain_remove → 置 alive = 0（懒删除）
  └─ 3. nCbs--
```

**站点本身保留**（还有其他回调）。

---

## 五、多实例与归属

### 5.1 站点表全局唯一

```c
struct ii_engine {
    ii_site** siteChunk[II_SITE_CHUNKS];   // 只有默认实例用
    int       nSites;
    unsigned long ownAddr[II_ENGINE_MAX_OWN];  // 非默认实例：记录自己装过哪些地址
    int       nOwn;
    int32_t   ownerId;
};
```

**为什么表唯一**：

`shell` 里硬编码的 `sid` 是**全局表索引**。如果每个实例有自己的表，`ii_dispatch` 在热路径上按"当前实例"查表就会错位 → `site_at` 返回 NULL → 回调永不执行。

### 5.2 归属标记

**每个回调节点记录 `owner`**：

```c
// 安装时
g_curOwner = (e == &g_defaultEngine) ? 0 : e->ownerId;
chain_insert(s, cb, user, priority);   // chain_insert 里 n->owner = g_curOwner

// 卸载时
int want = (e == &g_defaultEngine) ? 0 : e->ownerId;
for (ii_cb_node* n = s->cbs; n; n = n->next) {
    if (!n->alive || n->owner != want) continue;   // ← 只摘自己的
    chain_remove(s, n->cb, n->user);
}
```

**效果**：

```c
ii_engine* A = ii_engine_create();     // ownerId = 1000
ii_engine* B = ii_engine_create();     // ownerId = 1001

ii_engine_install(A, addr, cbA, NULL); // 站点在全局表，cbA.owner = 1000
ii_engine_install(B, addr, cbB, NULL); // 同一站点，cbB.owner = 1001
zh_hook("func", cbG, NULL);            // 同一站点，cbG.owner = 0

ii_engine_destroy(A);                  // 只摘 cbA，cbB 和 cbG 继续工作
```

### 5.3 站点何时真正卸载

```c
// ii_engine_uninstall 里
int hasLive = 0;
for (ii_cb_node* k = s->cbs; k; k = k->next)
    if (k->alive && k->cb) { hasLive = 1; break; }

if (!hasLive && !s->cb) {
    ii_uninstall(addr);   // 链上没活节点了，才真正卸站点
}
```

---

## 六、跨探针共享

### 6.1 场景

同一个进程里可能加载**多个探针 `.so`**（比如别的 Mod 也用了 Nexus）。

**问题**：各自有独立的静态变量 → A 注册的池 B 不知道。

**解决**：`zhreg` 用 `mmap` 一个共享文件（`poolreg.bin`）。

### 6.2 共享区结构

```c
typedef struct {
    volatile uint32_t lock;
    uint64_t          magic;
    int32_t           pid;
    uint64_t          procStartTime;
    ...
    zh_pool_rec       pool[ZHREG_MAX_POOL];   // 64 个池
    zh_site_rec       site[ZHREG_MAX_SITE];   // 2048 个站点
} zh_region;
```

**进程重启检测**：`pid + starttime` 不匹配 → 整块重置。

### 6.3 站点的共享副本

一次插桩会在两处登记：

1. **本地站点表**（`g_sites`）—— 热路径查询
2. **共享区**（`zhreg`）—— 跨探针可见

```c
if (g_reg) {
    s->regIdx = zhreg_add_site(g_reg, addr, s->orig, s->poolIdx, g_myId, 0);
    if (s->regIdx >= 0)
        zhreg_add_cb(g_reg, s->regIdx, (uint64_t)cb, (uint64_t)user, g_myId, s->priority);
}
```

**`ii_dispatch` 会同时遍历两处**：

```c
// 1. 本地链
for (ii_cb_node* n = s->cbs; n; n = n->next) { ... }

// 2. 共享区的外部回调（别的探针挂的）
if (s->regIdx >= 0 && g_reg) {
    if (zhreg_foreign_cb(g_reg, s->regIdx, g_myId) > 0) {
        for (int k = 0; k < zs->nCb; ++k) {
            if (!zs->cb[k].alive || zs->cb[k].owner == g_myId) continue;
            ((ii_callback)zs->cb[k].cb)(c, ...);
        }
    }
}
```

**`owner == g_myId` 跳过自己** —— 避免重复执行。

---

## 七、设计权衡

| 决策 | 原因 | 代价 |
|---|---|---|
| 站点表全局唯一 | shell 里的 sid 是绝对索引 | 多实例无法有独立表 |
| 懒删除回调 | 避免热路径 UAF | 节点会短暂堆积 |
| shell 槽位隔离 3 秒 | 避免跑进别人的 shell | 频繁装卸会暂时耗尽槽位 |
| 站点延迟 free 3 秒 | 避免 dispatch 读到已释放内存 | 内存短暂占用 |
| `movz` 装 sid | 单条指令 | 理论最多 65535 个站点 |
| 共享区 mmap 文件 | 跨探针通信 | 需要文件系统写权限 |

---

## 下一步

- [指令改写原理](11-Core-Instruction-Rewrite)
- [内存池与外壳](12-Core-Memory-Pool)
- [多实例 API](29-API-Multi-Instance)
