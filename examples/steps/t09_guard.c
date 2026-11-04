// 步骤 9：崩溃隔离（zh_hook_guarded）
//
// 在回调里故意解引用 NULL，验证：
//   1. 游戏不会被带崩
//   2. 隔离计数增加
//   3. 卸载正常
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        g_fd = open("/data/data/com.netease.x19/cache/zh_t09.txt",
                    O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (g_fd < 0) return;
    }
    char b[384]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++]='\n'; (void)!write(g_fd, b, n); }
}
static void CK(int c, const char* n, const char* d) {
    P("%s %-30s %s", c?"OK  ":"FAIL", n, d?d:"");
}

static volatile unsigned long g_hits = 0;

// ★ 故意崩溃的回调：解引用 NULL
static void cb_crash(ii_ctx* c, void* u) {
    (void)c; (void)u;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);
    volatile int* p = (volatile int*)0;
    *p = 1;                        // SIGSEGV
}

__attribute__((constructor))
static void run(void) {
    P("=== t09: 崩溃隔离 ===");

    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return; }
    unsigned long base = zh_module_base();

    const char* fn = "LocalPlayer_normalTick";
    unsigned long entry = zh_find(fn);
    if (!entry) { P("FAIL 找不到 %s", fn); return; }
    P("base=0x%lx  %s @ 0x%lx", base, fn, entry);

    long caughtBefore = zh_guard_caught();
    P("隔离计数(前) = %ld", caughtBefore);

    // ---- 1. 用带隔离的 hook ----
    int rc = zh_hook_guarded(fn, cb_crash, NULL);
    char d[96]; snprintf(d, sizeof d, "rc=%d (%s)", rc, ii_strerror(rc));
    CK(rc == 0, "zh_hook_guarded 安装", d);
    if (rc != 0) { P("  建议: %s", ii_strhint(rc)); return; }

    // ---- 2. 观察：回调会崩，但游戏不该崩 ----
    P("=== 观察 3 秒（回调每秒崩 20 次，游戏应无恙）===");
    for (int i = 1; i <= 3; ++i) {
        sleep(1);
        P("  t=%ds  hits=%lu  caught=%ld",
          i, __atomic_load_n(&g_hits, __ATOMIC_RELAXED), zh_guard_caught());
    }

    unsigned long hits = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    long caughtAfter = zh_guard_caught();

    snprintf(d, sizeof d, "%lu 次", hits);
    CK(hits > 0, "回调被触发（说明进程还活着）", d);

    snprintf(d, sizeof d, "%ld → %ld", caughtBefore, caughtAfter);
    CK(caughtAfter > caughtBefore, "隔离计数增加（崩溃被捕获）", d);

    // ---- 3. 卸载 ----
    int urc = zh_unhook(fn);
    snprintf(d, sizeof d, "rc=%d", urc);
    CK(urc == 0, "zh_unhook", d);

    // ---- 4. 卸载后仍能正常工作 ----
    P("=== 卸载后自检 ===");
    char st[128];
    zh_layer_status(st, sizeof st);
    P("  %s", st);
    CK(zh_layer_ready() == 1, "引擎仍就绪", "");

    P("=== t09 完成 ===");
}
