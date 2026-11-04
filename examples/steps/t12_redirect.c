// 步骤 12：调用重定向（对标 Mixin @Redirect）
//
// 在 ContainerScreenController_getStackFromModel 里，
// 把对 ItemStack_isNull 的调用重定向到我们自己的实现。
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "zh/zh_layer.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        g_fd = open("/data/data/com.netease.x19/cache/zh_t12.txt",
                    O_WRONLY|O_CREAT|O_APPEND, 0666);
        if (g_fd < 0) return;
    }
    char b[512]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++]='\n'; (void)!write(g_fd, b, n); }
}
static void CK(int c, const char* n, const char* d) {
    P("%s %-32s %s", c?"OK  ":"FAIL", n, d?d:"");
}

static volatile unsigned long g_hits = 0;
static volatile unsigned long g_retNull = 0;
static volatile unsigned long g_callSite = 0;      // 回调里拿到的（触发后才有）
static unsigned long g_installedSite = 0;          // 安装时算出的（一定有效）

// 重定向回调：返回 NULL 表示"走原实现"，返回值 = 要跳转的目标
//
// 这里做两件事：
//   1. 记账（证明被调用）
//   2. 第一次命中后返回 NULL，保持原行为，避免改变游戏逻辑
static void* on_redirect(ii_ctx* c, void* user) {
    (void)user;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);
    // 原目标地址可用于对照
    static int once = 0;
    if (__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED) == 0) {
        unsigned long orig = ii_redirect_orig_target(c->pc);
        __atomic_store_n(&g_retNull, orig, __ATOMIC_RELAXED);
        __atomic_store_n(&g_callSite, c->pc, __ATOMIC_RELAXED);
    }
    return NULL;    // NULL = 走原目标，不改变任何行为
}

static void* worker(void* arg) {
    (void)arg;
    P("");
    P("════════ t12 运行 #%ld ════════", (long)time(NULL));
    P("=== t12: 调用重定向 ===");

    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return NULL; }
    P("base=0x%lx", zh_module_base());

    const char* host = "ContainerScreenController_getStackFromModel";
    const char* target = "ItemStack_isNull";

    char d[192];
    unsigned long ha = zh_find(host);
    unsigned long ta = zh_find(target);
    snprintf(d, sizeof d, "host=0x%lx target=0x%lx", ha, ta);
    CK(ha && ta, "两个函数都在数据表里", d);
    if (!ha || !ta) return NULL;

    // ---- 0. 先算出调用点，卸载时要用（不能只依赖回调触发）----
    unsigned long callAddr = 0;
    int car = ii_call_site_addr(host, target, 0, &callAddr);
    snprintf(d, sizeof d, "rc=%d addr=0x%lx", car, callAddr);
    CK(car == 0 && callAddr, "ii_call_site_addr 定位调用点", d);
    g_installedSite = callAddr;

    // ---- 1. 安装重定向 ----
    int rc = zh_redirect(host, target, on_redirect, NULL);
    snprintf(d, sizeof d, "rc=%d (%s)", rc, ii_strerror(rc));
    CK(rc == 0, "zh_redirect 安装", d);
    if (rc != 0) { P("  提示: %s", ii_strhint(rc)); return NULL; }

    // ---- 2. 观察 ----
    P("=== 观察 6 秒（打开箱子/背包会触发）===");
    for (int i = 1; i <= 6; ++i) {
        sleep(1);
        P("  t=%ds  hits=%lu", i, __atomic_load_n(&g_hits, __ATOMIC_RELAXED));
    }

    unsigned long hits = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    snprintf(d, sizeof d, "%lu 次", hits);
    CK(hits == 0, "回调被触发（需开箱/背包）", hits ? d : "0 次（未触发业务）");

    unsigned long orig = __atomic_load_n(&g_retNull, __ATOMIC_RELAXED);
    if (orig) {
        snprintf(d, sizeof d, "0x%lx（应等于 target 0x%lx）", orig, ta);
        CK(orig == ta, "ii_redirect_orig_target 正确", d);
    } else {
        CK(1, "ii_redirect_orig_target", "未触发，跳过");
    }

    // ---- 3. 卸载（必须无条件执行）----
    //
    // ★ 关键：重定向站点把宿主函数里的 bl 改成了 b shell。
    //   即使回调从未触发（hits==0），这条 bl 也已经被改写。
    //   如果本库 dlclose 了而站点没撤，那个 b 就指向已卸载的 shell，
    //   宿主函数下次被调用时直接野跳 —— 真机表现就是几秒后卡死闪退，
    //   且没有 tombstone（跳的是已释放的代码页）。
    //
    //   所以这里用安装时算出的调用点地址卸载，不依赖回调是否触发过。
    unsigned long cs = g_callSite ? g_callSite : g_installedSite;
    snprintf(d, sizeof d, "调用点 0x%lx", cs);
    CK(cs != 0, "拿到调用点地址", d);
    if (cs) {
        int un = ii_uninstall(cs);
        snprintf(d, sizeof d, "rc=%d", un);
        CK(un == 0, "ii_uninstall 撤销重定向", d);
        int still = (ii_find(cs) != NULL);
        CK(!still, "重定向站点已移除", still ? "仍在" : "已清除");
    }

    // ---- 4. 双保险：确保进程里不残留任何本库装的站点 ----
    //    本库即将 dlclose，任何遗留站点都会变成野跳。
    int all = zh_unhook_all();
    snprintf(d, sizeof d, "rc=%d", all);
    CK(all >= 0, "zh_unhook_all 兜底清理", d);

    P("=== t12 完成 ===");
    return NULL;
}

// 异步：constructor 立刻返回，避免阻塞 dlopen 的调用线程（否则游戏 ANR）
__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
