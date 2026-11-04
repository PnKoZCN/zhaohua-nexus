# 核心概念：内存池与外壳

Hook 需要一小块可执行内存放 shell 和 trampoline。本页讲这块内存怎么来的。

---

## 一、需要多少内存

**一个站点需要**：

```
slot（SHELL_SIZE = 128 字节）
├── [0..27]    shell        28 字节
├── [28..31]   对齐填充
└── [32..75]   trampoline   最多 44 字节
```

**为什么是 128**：

| trampoline 模式 | 长度 | slot+32 后占用 |
|---|---|---|
| `ret` | 16 | 48 |
| 普通指令 + 相对跳回 | 20 | 52 |
| 无条件 `b` | 28 | 60 |
| 普通 + 远处跳回 | 28 | 60 |
| **条件分支**（`b.cond`/`cbz`/`tbz`） | **44** | **76** |

**历史 bug**：早期 `SHELL_SIZE = 64`，条件分支模式 44 字节从 `slot+32` 开始 → 到 `slot+76`，**越过槽位 12 字节**，踩到相邻槽位的 shell。

**现在 128，留足余量。**

---

## 二、内存从哪来（三条路）

### 2.1 路线优先级

```
alloc_shell_near(target)
  │
  ├─ 1. zhreg_alloc_slot()      ← 共享池里已有的空槽
  │     失败 ↓
  ├─ 2. scan_pool_for_target()  ← 扫描进程映射，补充候选
  │     失败 ↓
  └─ 3. pool_from_mmap_near()   ← 自己 mmap
        失败 ↓
      返回 NULL → 退 16 字节模式
```

### 2.2 路线 1：共享池

**`zhreg` 是一块 `mmap` 的共享文件**（`poolreg.bin`），跨探针可见。

```c
typedef struct {
    uint64_t base;      // 池基址
    uint64_t size;      // 池大小
    uint64_t used;      // 已分配到哪里
    ...
} zh_pool_rec;
```

**分配逻辑**：

```c
void* zhreg_alloc_slot(zh_region* r, uint64_t target, uint64_t slotSize,
                       int* poolIdxOut, int owner) {
    for (int i = 0; i < r->nPool; ++i) {
        zh_pool_rec* p = &r->pool[i];

        // 1. 优先复用已释放的槽位
        for (int k = 0; k < p->nFreeSlot; ++k) {
            uint64_t cand = p->freeSlot[k];
            int64_t d = (int64_t)cand - (int64_t)target;
            if (d <= -(1LL<<27) || d >= (1LL<<27)) continue;   // 太远
            // 拿走
        }

        // 2. 从 used 处切新的
        if (p->used + slotSize > p->size) continue;   // 满了
        uint64_t slot = p->base + p->used;
        int64_t d = (int64_t)slot - (int64_t)target;
        if (d <= -(1LL<<27) || d >= (1LL<<27)) continue;   // 太远
        p->used += slotSize;
        return (void*)slot;
    }
    return NULL;
}
```

**距离约束**：必须在 `±128 MB` 内（4 字节 `b` 的跳转范围）。

### 2.3 路线 2：扫描进程映射

**`poolhint` 表只有 5 条，可能不够用。** 这时扫描 `/proc/self/maps` 找候选：

```c
int scan_pool_for_target(uint64_t target) {
    FILE* f = fopen("/proc/self/maps", "r");
    while (fgets(line, ...)) {
        if (!strstr(path, "libminecraftpe")) continue;
        size_t len = b - a;
        if (len < 4096) continue;

        // ★ 只登记「匿名 + 可写」段
        //   r-xp（代码段）：写它会 SIGSEGV
        //   rw-p 但属于文件映射：写了会改文件内容
        if (strcmp(perm, "rw-p") != 0) continue;
        if (path[0] != '\0') continue;      // 必须是匿名段

        added += scan_region_into_pools(a, len, "anon", 64*1024);
    }
}
```

**`scan_region_into_pools` 的进一步过滤**：找**连续零区**或 **NOP 填充**：

```c
for (size_t off = 0; off + 4 <= len; off += 4) {
    uint32_t w;
    memcpy(&w, p + off, 4);
    int freeBlk = (w == 0 || w == 0xD503201Fu);   // 0 或 NOP
    ...
}
```

### 2.4 路线 3：自己 mmap

**前两条都失败时**：

```c
static int pool_from_mmap_near(uint64_t target) {
    static const size_t TRY_SIZES[] = {
        NEAR_POOL_SIZE, 4*1024*1024, 16*1024*1024, 64*1024*1024
    };

    for (int i = 0; i < sizeof TRY_SIZES/sizeof TRY_SIZES[0]; ++i) {
        size_t sz = TRY_SIZES[i];
        for (int t = 0; t < 64; ++t) {
            // 用 hint 参数让内核尽量靠近 target
            void* hint = (void*)((uintptr_t)(target + ((t & 1) ? (t+1)*STEP
                                                            : -(t+1)*STEP)) & ~0xFFFULL);
            void* p = mmap(hint, sz, PROT_READ|PROT_WRITE|PROT_EXEC,
                           MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) {
                p = mmap(NULL, sz, ...);
                if (p == MAP_FAILED) break;
            }

            int64_t d = (int64_t)(uintptr_t)p - (int64_t)target;
            if (d > -(1LL<<27) && d < (1LL<<27)) {
                // 命中！只用前 64 KB，其余还给系统
                if (sz > NEAR_POOL_SIZE)
                    munmap((char*)p + NEAR_POOL_SIZE, sz - NEAR_POOL_SIZE);
                zhreg_add_pool(g_reg, (uint64_t)p, NEAR_POOL_SIZE, "own", g_myId);
                return 1;
            }
            munmap(p, sz);
        }
    }
    return -1;
}
```

**实测结果**：游戏 `.so` 附近 ±128 MB 内**连 64 KB 空洞都没有**，这条路基本走不通。

**所以主要靠路线 1 和 2。**

---

## 三、池的类型与安全规则

### 3.1 三种池

| 类型 | `kind` | 来源 | 写 guard | 存活检测 |
|---|---|---|---|---|
| 自有 | `"own"` | `pool_from_mmap_near()` | ✅ 写 | 比对 guard |
| 提示 | `"hint"` | `poolhint` 表 | ❌ 不写 | `mincore()` |
| 扫描 | `"anon"`/`"bss"`/`"code"` | `scan_pool_for_target()` | ❌ 不写 | `mincore()` |

### 3.2 为什么只有 `own` 能写 guard

**写 guard 是为了检测"池是否被系统回收"**：

```c
p->guard = (uint32_t)(ZHREG_MAGIC ^ (base ^ (base >> 32)));
*(volatile uint32_t*)base = p->guard;   // ← 往池首写 4 字节
```

**但 `hint` / 扫描出来的池指向游戏自己的内存**：

- 写 `r-xp`（代码段）→ **SIGSEGV**
- 写 `rw-p`（数据段/堆）→ **改游戏数据**

**真机现象**：往 `hint` 池写 guard → 几秒后卡死闪退，**没有 tombstone**（崩的地方离这里很远，栈和 PC 都已损坏）。

### 3.3 修复后的规则

```c
// zhreg_add_pool 里
int ownMem = (kind && kind[0]=='o' && kind[1]=='w' && kind[2]=='n' && kind[3]==0);
p->noGuard = ownMem ? 0 : 1;
if (ownMem) {
    *(volatile uint32_t*)base = p->guard;   // 只有自己的内存才写
}
```

**存活检测分支**：

```c
int zhreg_pool_alive(zh_region* r, int idx) {
    zh_pool_rec* p = &r->pool[idx];
    if (p->noGuard) {
        // 只读检测：页是否还在映射里
        unsigned char vec = 0;
        if ((uintptr_t)p->base & 0xFFF) return 0;
        if (mincore((void*)p->base, 4096, &vec) != 0) return 0;
        return 1;
    }
    return (*(volatile uint32_t*)p->base == p->guard) ? 1 : 0;
}
```

---

## 四、槽位的生命周期

### 4.1 安装：拿走槽位

```c
void* slot = zhreg_alloc_slot(g_reg, target, SHELL_SIZE, &pi, g_myId);
// slot      = 槽位地址
// pi        = 池索引
```

### 4.2 卸载：归还槽位（隔离 3 秒）

```c
// site_release 里
if (s->shell) zhreg_free_slot(g_reg, s->poolIdx, (uint64_t)s->shell);
```

**`zhreg_free_slot` 不是立刻可复用**：

```c
int zhreg_free_slot(zh_region* r, int poolIdx, uint64_t slotAddr) {
    ...
    // ★ 先进隔离区，记录归还时刻
    p->pendSlot[p->nPend] = slotAddr;
    p->pendMs[p->nPend]   = now;
    p->nPend++;
}
```

**为什么**：

卸载时用 4 字节原子写恢复了入口指令，但**此刻可能还有线程已经跳进了 shell、还没执行完**。

如果槽位立刻被下一次 install 拿去写新代码，那些线程就会跑进**别人的 shell** → 参数错位、死循环、崩溃。

**隔离期**：`ZHREG_QUARANTINE_MS = 3000`（3 秒）。

### 4.3 隔离区结算

**没有独立线程，在下一次分配时顺带做**：

```c
static void quarantine_drain(zh_pool_rec* p, uint64_t now) {
    for (int k = 0; k < p->nPend; ) {
        if (now - p->pendMs[k] < ZHREG_QUARANTINE_MS) { ++k; continue; }
        uint64_t a = p->pendSlot[k];
        // 从隔离区移除
        p->pendSlot[k] = p->pendSlot[--p->nPend];
        p->pendMs[k]   = p->pendMs[p->nPend];
        // 转入 freeSlot（满了就丢弃）
        if (p->nFreeSlot < ZHREG_MAX_FREE)
            p->freeSlot[p->nFreeSlot++] = a;
    }
}
```

在 `zhreg_alloc_slot` 开头调用：

```c
for (int i = 0; i < r->nPool; ++i) {
    if (p->nPend > 0) quarantine_drain(p, now0);
    ...
}
```

**代价**：频繁装卸会暂时耗尽槽位（要等 3 秒）。

---

## 五、容量规划

### 5.1 各池容量

| 池 | 大小 | 槽位数 |
|---|---|---|
| `hint` #0 | 308 KB | 2,464 |
| `hint` #1 | 244 KB | 1,952 |
| `hint` #2 | 20 KB | 160 |
| `hint` #3 | 8 KB | 64 |
| `hint` #4 | 7 KB | 56 |
| **合计** | **587 KB** | **约 4,700** |

**（SHELL_SIZE = 128 时）**

### 5.2 站点上限

```c
#define II_MAX_SITES (II_SITE_CHUNK * II_SITE_CHUNKS)   // 256 × 1024 = 262,144
```

**但实际受两个限制**：

1. `movz` 只能装 16 位 → **`sid` 最大 65,535**
2. 池容量 → **约 4,700 个 shell**

**所以实际可用约 4,700 个站点。**

### 5.3 用完了怎么办

```c
int rc = zh_hook(...);
if (rc == -3) {
    // 站点表满 或 shell 分配失败
    // 卸载一些不用的 hook
    zh_unhook("不常用的函数");
    // 重试
}
```

---

## 六、跨探针共享

### 6.1 共享区文件

```c
#define ZHREG_PATH "/data/data/com.netease.x19/files/zhprobe/poolreg.bin"
#define ZHREG_FILE_SIZE (512*1024)
```

**`mmap` 方式**：

```c
int fd = open(ZHREG_PATH, O_RDWR | O_CREAT, 0666);
void* m = mmap(NULL, ZHREG_FILE_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
close(fd);
```

### 6.2 进程重启检测

```c
uint64_t st22 = proc_starttime();   // /proc/self/stat 第 22 字段
int32_t  pid  = getpid();

if (r->magic != ZHREG_MAGIC || r->pid != pid || r->procStartTime != st22) {
    memset(r, 0, sizeof(zh_region));   // 整块重置
    r->magic = ZHREG_MAGIC;
    r->pid = pid;
    r->procStartTime = st22;
}
```

**为什么**：上次进程的池地址在新进程里无效（ASLR）。

### 6.3 并发保护

```c
static void zhreg_lock(zh_region* r) {
    while (__sync_lock_test_and_set(&r->lock, 1)) {
        while (r->lock) sched_yield();
    }
}
```

**简单的自旋锁**（临界区很短）。

---

## 七、池失效与恢复

### 7.1 什么情况池会失效

| 情况 | 检测方式 |
|---|---|
| 系统回收了 `hint` 池所在页 | `mincore()` 失败 |
| 游戏重新映射了该区域 | guard 不匹配 / `mincore` 失败 |
| 静态标记为 dead | `state == 2` |

### 7.2 watchdog 的恢复

**每 200 ms 检查一次**（`watchdog_main`）：

```c
for (int i = 0; g_reg && i < g_reg->nPool; ++i) {
    zh_pool_rec* q = &g_reg->pool[i];
    if (q->state == 0) continue;

    int dead = (q->state == 2) || !zhreg_pool_alive(g_reg, i);
    if (!dead) continue;

    // 只处理自己还有站点留在这里的情况
    int mine = 0;
    for (int k = 0; k < q->nSite; ++k)
        if (q->siteAddr[k] && ii_find(q->siteAddr[k])) { mine = 1; break; }
    if (!mine) continue;

    wd_log("POOL DEAD #%d base=0x%llx sites=%d\n", i, q->base, q->nSite);
    recover_pool(i);
}
```

**`recover_pool` 做什么**：把该池里的站点迁移到新池（重新分配 shell + 重新 patch）。

---

## 八、诊断

### 8.1 查看池状态

```c
ii_engine_stats st;
ii_engine_get_stats(ii_default(), &st);
LOG_I(LOG_POOL, "站点 %d，池死亡 %ld，恢复 %ld",
      st.nSites, st.poolDeaths, st.poolRecoveries);
```

### 8.2 日志标记

```
hint 池 + vaddr 0x1334b08c 308 KB      安装时注册池
near 池 @0x7c3a108010 覆盖 target=...   自己 mmap 成功
POOL DEAD #3 base=0x...                 池失效
recover(#3) -> 0                        恢复成功
[alloc_shell] target=... slot=... pool=3   分配记录
```

### 8.3 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| `alloc_shell` 一直返回 NULL | 池全满/全失效 | 卸载不用的 hook |
| `POOL DEAD` 频繁 | `hint` 区域被回收 | 换更大的 `hint` 池 |
| `-15` 错误 | shell 分配失败 | 同上 |
| 卸载后立即装同函数失败 | 槽位在隔离区 | 等 3 秒 |

---

## 九、设计权衡

| 决策 | 理由 | 代价 |
|---|---|---|
| 用游戏 `.bss` 做池 | ±128 MB 内唯一来源 | 有被回收的风险 |
| hint 池不写 guard | 写它就是改游戏内存 | 存活检测只能用 `mincore` |
| 槽位隔离 3 秒 | 防跑进别人的 shell | 频繁装卸会耗尽 |
| 优先用共享池 | 跨探针复用 | 需要文件系统 |
| 40 个池上限 | 共享区大小限制 | 极端场景不够 |

---

## 下一步

- [指令改写原理](11-Core-Instruction-Rewrite)
- [站点与回调链](10-Core-Site-And-Chain)
- [性能与开销](41-Reference-Performance)
