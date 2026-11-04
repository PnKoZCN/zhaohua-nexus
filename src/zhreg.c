#include "zh/zhreg.h"
#include "zh/zlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>

static zh_region* g_region = NULL;
static int        g_owner  = 0;

// ---------------------------------------------------------------------------
// 进程身份：pid + starttime。后者能区分「pid 被复用」的情况。
// ---------------------------------------------------------------------------
static uint64_t proc_starttime(void) {
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = 0;

    // 第 2 字段是 (comm)，可能含空格和括号 → 从最后一个 ')' 之后开始数
    char* p = strrchr(buf, ')');
    if (!p) return 0;
    ++p;

    // 现在 p 指向第 3 字段之前。需要第 22 字段（starttime）
    // 从第 3 字段数到第 22 字段 = 19 次跳过
    for (int field = 3; field < 22; ++field) {
        while (*p == ' ') ++p;
        while (*p && *p != ' ') ++p;
        if (!*p) return 0;
    }
    while (*p == ' ') ++p;
    return strtoull(p, NULL, 10);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

// ---------------------------------------------------------------------------
// 附加
// ---------------------------------------------------------------------------
zh_region* zhreg_attach(void) {
    if (g_region) return g_region;

    int fd = open(ZHREG_PATH, O_RDWR | O_CREAT, 0666);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < ZHREG_FILE_SIZE) {
        if (ftruncate(fd, ZHREG_FILE_SIZE) != 0) { close(fd); return NULL; }
    }

    void* m = mmap(NULL, ZHREG_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return NULL;

    zh_region* r = (zh_region*)m;
    uint64_t st22 = proc_starttime();
    int32_t  pid  = (int32_t)getpid();

    // 进程重启（或首次）→ 整块重置，旧地址全部失效
    if (r->magic != ZHREG_MAGIC || r->pid != pid || r->procStartTime != st22) {
        memset(r, 0, sizeof(zh_region));
        r->magic         = ZHREG_MAGIC;
        r->pid           = pid;
        r->procStartTime = st22;
        r->wdOwner       = 0;
        r->wdHeartbeat   = 0;
        r->nOwner        = 0;
        r->nPool         = 0;
        r->nSite         = 0;
    }

    g_region = r;

    // 分配本探针实例 ID（从 1 开始）
    __sync_lock_test_and_set(&r->lock, 1);
    if (r->nOwner < 1) r->nOwner = 1;
    g_owner = r->nOwner++;
    __sync_lock_release(&r->lock);
    return r;
}

int zhreg_owner(void) { return g_owner; }

// ---------------------------------------------------------------------------
// 锁：跨 so 但同进程，原子交换足够。带自旋上限防止卡死。
// ---------------------------------------------------------------------------
void zhreg_lock(zh_region* r) {
    if (!r) return;
    int spins = 0;
    while (__sync_lock_test_and_set(&r->lock, 1)) {
        if (++spins > 3000000) break;   // 强制继续，宁可冒险也不卡死整个进程
        sched_yield();
    }
}

void zhreg_unlock(zh_region* r) {
    if (r) __sync_lock_release(&r->lock);
}

// ---------------------------------------------------------------------------
// 池
// ---------------------------------------------------------------------------
int zhreg_add_pool(zh_region* r, uint64_t base, uint64_t size, const char* kind, int owner) {
    if (!r) return -1;
    size &= ~0xFFFULL;
    if (size < 4096) size = 4096;      // 小于一页的对齐到一页

    zhreg_lock(r);
    for (int i = 0; i < r->nPool; ++i)
        if (r->pool[i].base == base && r->pool[i].state != 2) {
            // ★ 复用：池是跨探针共享资源。返回已有索引，让后加载的探针也用它，
            //   避免拿不到池时降级到 wideMode（16 字节非原子改写 + suspend 全部线程，
            //   连续多次会把游戏拖死）。slot 由共享的 p->used 原子分配，不会撞车。
            zhreg_unlock(r); return i;
        }
    if (r->nPool >= ZHREG_MAX_POOL) { zhreg_unlock(r); return -3; }

    int idx = r->nPool++;
    zh_pool_rec* p = &r->pool[idx];
    memset(p, 0, sizeof(*p));
    p->base  = base;
    p->size  = size;
    p->used  = 16;
    p->guard = (uint32_t)(ZHREG_MAGIC ^ (base ^ (base >> 32)));
    p->state = 1;
    p->owner = owner;
    if (kind) strncpy(p->kind, kind, sizeof(p->kind) - 1);

    // ★ guard 标记只在「池是我们自己 mmap 的」时才写。
    //
    // 池分两类：
    //   near/anon —— pool_from_mmap_near() 自己 mmap 的匿名页，
    //                这一段地址归我们独占，写 4 字节 guard 完全安全。
    //   hint      —— 来自 poolhint 表，指向游戏 .so 里的零填充区
    //                （.bss 尾部 / 代码段对齐空隙）。那些字节虽然
    //                此刻是 0，但没有任何机制保证游戏永远不用它们；
    //                往里写 guard 等于改游戏内存，崩的地方离这里很远，
    //                真机表现为「几秒后卡死闪退、没有 tombstone」。
    //
    // 判定依据：kind 是否为 "own" —— 只有 pool_from_mmap_near() 自己
    // mmap 出来的池才用这个标记。其余（hint / anon / bss / code / reuse）
    // 全是进程里已有的映射，一律不写 guard。
    //   "own"  : 我们 mmap 的，独占，可以写 guard
    //   其他   : 别人的映射，只读检测（mincore），绝不写
    int ownMem = (kind && kind[0]=='o' && kind[1]=='w' && kind[2]=='n' && kind[3]==0);
    p->noGuard = ownMem ? 0 : 1;
    if (ownMem) {
        *(volatile uint32_t*)base = p->guard;
        __builtin___clear_cache((char*)base, (char*)base + 4);
    }
    zhreg_unlock(r);
    return idx;
}

static void quarantine_drain(zh_pool_rec* p, uint64_t now);

void* zhreg_alloc_slot(zh_region* r, uint64_t target, uint64_t slotSize,
                       int* poolIdxOut, int owner) {
    if (poolIdxOut) *poolIdxOut = -1;
    if (!r) return NULL;
    (void)owner;

    zhreg_lock(r);
    int nFull = 0, nFar = 0;

    // 先结算隔离区：隔离期已满的槽位转回可复用
    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    uint64_t now0 = (uint64_t)ts0.tv_sec * 1000u + (uint64_t)ts0.tv_nsec / 1000000u;

    for (int i = 0; i < r->nPool; ++i) {
        zh_pool_rec* p = &r->pool[i];
        if (p->state != 1) continue;
        if (p->nPend > 0) quarantine_drain(p, now0);

        // 优先复用已释放的槽位，避免反复装卸把 used 顶到池尾
        for (int k = 0; k < p->nFreeSlot; ++k) {
            uint64_t cand = p->freeSlot[k];
            int64_t d = (int64_t)cand - (int64_t)target;
            if (d <= -(1LL << 27) || d >= (1LL << 27)) continue;   // 太远，换一个
            p->freeSlot[k] = p->freeSlot[--p->nFreeSlot];          // 交换删除
            if (poolIdxOut) *poolIdxOut = i;
            zhreg_unlock(r);
            return (void*)cand;
        }

        if (p->used + slotSize > p->size) { ++nFull; continue; }
        uint64_t slot = p->base + p->used;
        int64_t d = (int64_t)slot - (int64_t)target;
        if (d <= -(1LL << 27) || d >= (1LL << 27)) { ++nFar; continue; }
        p->used += slotSize;
        if (poolIdxOut) *poolIdxOut = i;
        zhreg_unlock(r);
        return (void*)slot;
    }
    zhreg_unlock(r);
    LOG_W(LOG_REG, "alloc 失败 target=0x%llx 池共%d 满%d 太远%d",
          (unsigned long long)target, r->nPool, nFull, nFar);
    return NULL;
}

int zhreg_note_site(zh_region* r, int poolIdx, unsigned long addr) {
    if (!r || poolIdx < 0 || poolIdx >= r->nPool) return -1;
    zhreg_lock(r);
    zh_pool_rec* p = &r->pool[poolIdx];
    for (int i = 0; i < p->nSite && i < ZHREG_SITES_PER_POOL; ++i)
        if (p->siteAddr[i] == 0) { p->siteAddr[i] = addr; zhreg_unlock(r); return 0; }
    if (p->nSite < ZHREG_SITES_PER_POOL) {
        p->siteAddr[p->nSite++] = addr;
        zhreg_unlock(r); return 0;
    }
    zhreg_unlock(r);
    return -2;
}

// 页是否仍然映射。
//
// 池所在的映射可能已经消失（游戏热更新 .so、模块被 unload）。
// 那时直接读 p->base 会 SIGSEGV —— watchdog 自己先崩，
// 而不是去迁移站点。mincore 对未映射地址返回 ENOMEM 而不触发异常。
static int zhreg_page_mapped(const void* addr) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    uintptr_t p = (uintptr_t)addr & ~((uintptr_t)ps - 1);
    unsigned char vec = 0;
    return mincore((void*)p, (size_t)ps, &vec) == 0;
}

// 归还槽位：site_release 时调用
int zhreg_free_slot(zh_region* r, int poolIdx, uint64_t slotAddr) {
    if (!r || poolIdx < 0 || poolIdx >= r->nPool || !slotAddr) return -1;
    zhreg_lock(r);
    zh_pool_rec* p = &r->pool[poolIdx];

    // ★ 不是直接进 freeSlot，而是先进隔离区（记录归还时刻）。
    //   原因见 zhreg.h 里 pendSlot 的说明：此刻可能还有线程正在执行
    //   这个 shell 里的代码，立刻复用会让它们跑进新 shell。
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;

    // 去重（可能已在隔离区，也可能已在 freeSlot）
    for (int k = 0; k < p->nPend; ++k)
        if (p->pendSlot[k] == slotAddr) { zhreg_unlock(r); return 0; }
    for (int k = 0; k < p->nFreeSlot; ++k)
        if (p->freeSlot[k] == slotAddr) { zhreg_unlock(r); return 0; }

    if (p->nPend < ZHREG_MAX_PENDING) {
        p->pendSlot[p->nPend] = slotAddr;
        p->pendMs[p->nPend]   = now;
        p->nPend++;
        zhreg_unlock(r);
        return 0;
    }
    zhreg_unlock(r);
    return -2;      // 隔离区满：放弃回收（used 不回退，下次 new 分配）
}

// 把隔离期已满的槽位转入 freeSlot（在 alloc 时顺带做，不需要线程）
static void quarantine_drain(zh_pool_rec* p, uint64_t now) {
    for (int k = 0; k < p->nPend; ) {
        if (now - p->pendMs[k] < ZHREG_QUARANTINE_MS) { ++k; continue; }
        uint64_t a = p->pendSlot[k];
        // 从隔离区移除
        p->pendSlot[k] = p->pendSlot[--p->nPend];
        p->pendMs[k]   = p->pendMs[p->nPend];
        // 转入 freeSlot（满了就丢弃，used 不回退）
        int dup = 0;
        for (int j = 0; j < p->nFreeSlot; ++j)
            if (p->freeSlot[j] == a) { dup = 1; break; }
        if (!dup && p->nFreeSlot < ZHREG_MAX_FREE)
            p->freeSlot[p->nFreeSlot++] = a;
    }
}

int zhreg_pool_alive(zh_region* r, int idx) {
    if (!r || idx < 0 || idx >= r->nPool) return 0;
    zh_pool_rec* p = &r->pool[idx];
    if (p->state != 1) return 0;
    if (!p->base || p->size < 4) return 0;
    if (!zhreg_page_mapped((void*)p->base)) return 0;   // 映射没了 → 判定死亡
    if (p->noGuard) {
        // 不写 guard 的池（hint）：检查这段地址是否仍可读且非映射消失。
        // 用 mincore 判断页是否仍在内存里，不改写任何字节。
        unsigned char vec = 0;
        if ((uintptr_t)p->base & 0xFFF) return 0;      // 非页对齐，视为失效
        if (mincore((void*)p->base, 4096, &vec) != 0) return 0;
        return 1;
    }
    return (*(volatile uint32_t*)p->base == p->guard) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// watchdog 归属：一个进程只该有一个在跑。心跳超时允许别人接管。
// ---------------------------------------------------------------------------
int zhreg_wd_claim(zh_region* r, int owner) {
    if (!r) return 0;
    int ok = (int)__sync_bool_compare_and_swap(&r->wdOwner, 0u, (uint32_t)owner);
    if (ok) r->wdHeartbeat = now_ms();
    return ok;
}

void zhreg_wd_beat(zh_region* r) {
    if (r) r->wdHeartbeat = now_ms();
}

int zhreg_wd_should_takeover(zh_region* r) {
    if (!r) return 0;
    uint64_t hb = r->wdHeartbeat;
    if (hb == 0) return 1;
    return (now_ms() - hb) > ZHREG_WD_TIMEOUT_MS ? 1 : 0;
}

int zhreg_wd_takeover(zh_region* r, int owner) {
    if (!r) return 0;
    if (!zhreg_wd_should_takeover(r)) return 0;
    r->wdOwner     = (uint32_t)owner;
    r->wdHeartbeat = now_ms();
    return 1;
}

int zhreg_wd_is_mine(zh_region* r, int owner) {
    if (!r) return 0;
    return ((int)r->wdOwner == owner) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 站点
// ---------------------------------------------------------------------------
int zhreg_add_site(zh_region* r, unsigned long addr, uint32_t orig, int poolIdx,
                    int owner, int kind) {
    if (!r) return -1;
    zhreg_lock(r);
    for (int i = 0; i < r->nSite; ++i)
        if (r->site[i].addr == addr && r->site[i].alive) { zhreg_unlock(r); return -2; }
    // 复用已死的槽位：nSite 只增不减会让历史记录永久占表，
    // 长跑场景（反复装/卸）最终撑到 ZHREG_MAX_SITE。
    int idx = -1;
    for (int i = 0; i < r->nSite; ++i)
        if (!r->site[i].alive) { idx = i; break; }
    if (idx < 0) {
        if (r->nSite >= ZHREG_MAX_SITE) { zhreg_unlock(r); return -3; }
        idx = r->nSite++;
    }
    zh_site_rec* s = &r->site[idx];
    memset(s, 0, sizeof(*s));
    s->addr    = addr;
    s->orig    = orig;
    s->owner   = owner;
    s->poolIdx = poolIdx;
    s->alive   = 1;
    s->kind    = kind;
    s->nCb     = 0;
    zhreg_unlock(r);
    return idx;
}

// 查地址是否已有活站点
int zhreg_find_site(zh_region* r, unsigned long addr) {
    if (!r) return -1;
    zhreg_lock(r);
    int found = -1;
    for (int i = 0; i < r->nSite; ++i)
        if (r->site[i].addr == addr && r->site[i].alive) { found = i; break; }
    zhreg_unlock(r);
    return found;
}

// 往已有站点追加回调 —— 多探针共存的关键
//
// 同一个地址第二个探针来装时，不再返回 -14 拒绝，而是挂到回调链上。
// 前提是 handler 类型必须一致（FULL 的 ii_ctx 和 LIGHT 的 ii_ctx_light 布局不同，
// 混挂会让回调读到错位的寄存器）。
int zhreg_add_cb(zh_region* r, int siteIdx, uint64_t cb, uint64_t user,
                  int32_t owner, int32_t priority) {
    if (!r || siteIdx < 0 || siteIdx >= r->nSite) return -2;
    zhreg_lock(r);
    zh_site_rec* s = &r->site[siteIdx];
    if (!s->alive) { zhreg_unlock(r); return -2; }
    // 同一探针重复挂同一个函数 → 视为幂等
    for (int i = 0; i < s->nCb; ++i)
        if (s->cb[i].alive && s->cb[i].owner == owner && s->cb[i].cb == cb) {
            zhreg_unlock(r); return i;
        }
    // 找空槽
    int slot = -1;
    for (int i = 0; i < ZHREG_MAX_CB; ++i) if (!s->cb[i].alive) { slot = i; break; }
    if (slot < 0) { zhreg_unlock(r); return -3; }

    s->cb[slot].cb       = cb;
    s->cb[slot].user     = user;
    s->cb[slot].owner    = owner;
    s->cb[slot].priority = priority;
    __atomic_store_n(&s->cb[slot].alive, 1, __ATOMIC_RELEASE);

    // 按 priority 降序整理：priority 大的先执行（对应 Mixin 的高优先级先跑）
    // 链很短（≤4），插入排序足够
    for (int i = 0; i < ZHREG_MAX_CB; ++i) {
        for (int j = 0; j + 1 < ZHREG_MAX_CB; ++j) {
            if (!s->cb[j].alive) continue;
            if (!s->cb[j+1].alive) continue;
            if (s->cb[j].priority < s->cb[j+1].priority) {
                zh_cb_rec t = s->cb[j];
                s->cb[j] = s->cb[j+1];
                s->cb[j+1] = t;
            }
        }
    }
    int n = 0;
    for (int i = 0; i < ZHREG_MAX_CB; ++i) if (s->cb[i].alive) n = i + 1;
    __atomic_store_n(&s->nCb, n, __ATOMIC_RELEASE);
    zhreg_unlock(r);
    return slot;
}

void zhreg_del_cb(zh_region* r, int siteIdx, int32_t owner) {
    if (!r || siteIdx < 0 || siteIdx >= r->nSite) return;
    zhreg_lock(r);
    zh_site_rec* s = &r->site[siteIdx];
    for (int i = 0; i < s->nCb; ++i)
        if (s->cb[i].owner == owner)
            __atomic_store_n(&s->cb[i].alive, 0, __ATOMIC_RELEASE);
    while (s->nCb > 0 && !__atomic_load_n(&s->cb[s->nCb-1].alive, __ATOMIC_ACQUIRE))
        --s->nCb;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    zhreg_unlock(r);
}

// 属于别人的活回调数量：为 0 时 handler 走纯本地快路径，不碰共享区
int zhreg_foreign_cb(zh_region* r, int siteIdx, int32_t myId) {
    if (!r || siteIdx < 0 || siteIdx >= r->nSite) return 0;
    zh_site_rec* s = &r->site[siteIdx];
    // 热路径（每次命中都走），不能加锁。这里只做原子加载：
    // 32 位对齐读本身不会撕裂，加 acquire 是为了看到 add_cb/del_cb 的最新值。
    // 极端竞争下最多漏调一次回调（下次就对了），不会读到坏指针 ——
    // add_cb 是先写 cb/user/owner 再写 alive=1，del_cb 是先写 alive=0。
    int nCb = __atomic_load_n(&s->nCb, __ATOMIC_ACQUIRE);
    if (nCb > ZHREG_MAX_CB) nCb = ZHREG_MAX_CB;
    int n = 0;
    for (int i = 0; i < nCb; ++i)
        if (__atomic_load_n(&s->cb[i].alive, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&s->cb[i].owner, __ATOMIC_RELAXED) != myId) ++n;
    return n;
}

void zhreg_del_site(zh_region* r, unsigned long addr, int owner) {
    if (!r) return;
    zhreg_lock(r);
    // 同时匹配 alive：同一地址可能残留已死的旧记录，
    // 只按 addr 会命中旧记录、把真正活着的那条留在表里。
    for (int i = 0; i < r->nSite; ++i)
        if (r->site[i].addr == addr && r->site[i].alive &&
            (owner <= 0 || r->site[i].owner == owner)) {
            r->site[i].alive = 0;
            break;
        }
    zhreg_unlock(r);
}
