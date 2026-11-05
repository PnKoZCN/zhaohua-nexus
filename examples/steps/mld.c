// mld：诊断探针 —— 把共享注册区的真实内容 dump 出来
//
// 目的：不再靠猜。直接 mmap poolreg.bin，按 zh_region 布局解析，
//       打印每个站点的地址/归属/是否活着/几个回调，以及回调槽的 owner。
//       同时挂几个 tick 候选函数，看哪个在当前游戏状态下真的会被调用。
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include "zh/zh_layer.h"
#include "zh/zhreg.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        g_fd = open("/data/data/com.netease.x19/cache/zh_mlD.txt",
                    O_WRONLY|O_CREAT|O_APPEND, 0666);
        if (g_fd < 0) return;
    }
    char b[400]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++] = '\n'; (void)!write(g_fd, b, n); }
}

// ── 共享区直读 ─────────────────────────────────────────
static zh_region* open_region(void) {
    int fd = open(ZHREG_PATH, O_RDONLY);
    if (fd < 0) { P("  [region] 打不开 %s errno=%d", ZHREG_PATH, errno); return NULL; }
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(zh_region)) {
        P("  [region] 文件太小 %lld < %zu", (long long)st.st_size, sizeof(zh_region));
        close(fd); return NULL;
    }
    void* m = mmap(NULL, sizeof(zh_region), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { P("  [region] mmap 失败"); return NULL; }
    return (zh_region*)m;
}

static void dump_region(void) {
    zh_region* r = open_region();
    if (!r) return;

    P("  ── 共享区 ──");
    P("  magic=0x%llx (本地期望 0x%llx | layout<<32)",
      (unsigned long long)r->magic, (unsigned long long)ZHREG_MAGIC);
    P("  pid=%d (本进程 %d)  进程校验=%llu",
      r->pid, (int)getpid(), (unsigned long long)r->procStartTime);
    P("  sizeof(zh_region)=%zu  nOwner=%d nPool=%d nSite=%d",
      sizeof(zh_region), r->nOwner, r->nPool, r->nSite);

    int shown = 0;
    for (int i = 0; i < r->nSite && shown < 12; ++i) {
        zh_site_rec* s = &r->site[i];
        if (!s->addr) continue;
        P("  site[%d] addr=0x%lx owner=%d alive=%d kind=%d nCb=%d",
          i, s->addr, s->owner, s->alive, s->kind, s->nCb);
        for (int k = 0; k < ZHREG_MAX_CB; ++k) {
            if (!s->cb[k].alive) continue;
            P("      cb[%d] fn=0x%llx owner=%d prio=%d",
              k, (unsigned long long)s->cb[k].cb,
              s->cb[k].owner, s->cb[k].priority);
        }
        ++shown;
    }
    if (!shown) P("  （共享区里没有任何站点）");

    for (int i = 0; i < r->nPool && i < 8; ++i) {
        zh_pool_rec* p = &r->pool[i];
        if (!p->base) continue;
        P("  pool[%d] base=0x%llx size=%llu used=%llu state=%d kind=%s",
          i, (unsigned long long)p->base, (unsigned long long)p->size,
          (unsigned long long)p->used, p->state, p->kind);
    }
    munmap(r, sizeof(zh_region));
}

// ── tick 候选 ──────────────────────────────────────────
#define NCAND 5
static const char* kCand[NCAND] = {
    "LocalPlayer_normalTick",
    "ClientInputUpdateSystem_tick",
    "CameraAvoidanceSystem_tick",
    "ContainerScreenController_tick",
    "Actor_statusFlagPostUpdate",
};
static volatile unsigned long g_n[NCAND];

#define MKCB(idx) \
    static void cb##idx(ii_ctx* c, void* u) { \
        (void)c; (void)u; \
        __atomic_add_fetch(&g_n[idx], 1, __ATOMIC_RELAXED); \
    }
MKCB(0) MKCB(1) MKCB(2) MKCB(3) MKCB(4)

static void* kCbFn[NCAND] = { (void*)cb0, (void*)cb1, (void*)cb2,(void*)cb3,(void*)cb4 };

static void* worker(void* arg) {
    (void)arg;
    P("");
    P("════ 诊断探针 ════");
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) {
        P("FAIL 引擎初始化"); return NULL;
    }
    P("  ii_count=%d（清场前）", ii_count());
    zh_unhook_all();
    P("  ii_count=%d（清场后）", ii_count());

    for (int i = 0; i < NCAND; ++i) {
        unsigned long a = zh_find(kCand[i]);
        P("  find %-32s = 0x%lx", kCand[i], a);
        if (!a) continue;
        int r = zh_hook_owned(kCand[i], (ii_callback)kCbFn[i], NULL, "diag");
        P("       hook rc=%d 本地链=%d", r, ii_callback_count(a));
    }

    // ★ 关键自检：直接在进程内读入口指令，和引擎记录的 orig 比对。
    //   相同 = 补丁没写进去；不同 = 补丁生效了，那问题就在 dispatch。
    P("  ── 入口补丁自检 ──");
    for (int i = 0; i < NCAND; ++i) {
        unsigned long a = zh_find(kCand[i]);
        if (!a) continue;
        ii_site* s = ii_find(a);
        uint32_t now = *(volatile uint32_t*)a;
        P("  %-32s live=0x%08x orig=0x%08x shell=0x%lx tramp=0x%lx wide=%d inst=%d %s",
          kCand[i], now,
          s ? s->orig : 0,
          s ? (unsigned long)s->shell : 0UL,
          s ? (unsigned long)s->trampoline : 0UL,
          s ? s->wideMode : -1,
          s ? s->installed : -1,
          (s && now != s->orig) ? "★已打补丁" : "✗未打补丁");
    }

    P("  ── 安装后立刻 dump ──");
    dump_region();

    for (int t = 0; t < 6; ++t) {
        sleep(5);
        P("  [dispatch] calls=%lu miss=%lu",
          ii_dispatch_count(), ii_dispatch_miss());
        P("  t=%2ds  LP=%lu input=%lu cam=%lu cont=%lu actor=%lu",
          (t+1)*5,
          __atomic_load_n(&g_n[0], __ATOMIC_RELAXED),
          __atomic_load_n(&g_n[1], __ATOMIC_RELAXED),
          __atomic_load_n(&g_n[2], __ATOMIC_RELAXED),
          __atomic_load_n(&g_n[3], __ATOMIC_RELAXED),
          __atomic_load_n(&g_n[4], __ATOMIC_RELAXED));
    }

    P("  ── 30 秒后 dump ──");
    dump_region();
    P("════ 结束 ════");
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
