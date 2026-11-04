// 步骤 13：常量修改（对标 Mixin @ModifyConstant）
//
// 在 Actor_getPos 里某条编码了特定常量值的指令处插桩，
// 回调里读该寄存器 —— 证明能精确定位到常量指令。
//
// 注意：本测试只读不改，改变游戏常量会直接影响逻辑。
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include "zh/zh_layer.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        g_fd = open("/data/data/com.netease.x19/cache/zh_t13.txt",
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

// 只记账，不改任何寄存器 —— 保证游戏行为不变
static void on_const(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);
}

__attribute__((constructor))
static void run(void) {
    P("");
    P("════════ t13 运行 #%ld ════════", (long)time(NULL));
    P("=== t13: 常量修改 ===");

    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return; }
    P("base=0x%lx", zh_module_base());

    const char* fn = "LocalPlayer_normalTick";
    unsigned long a = zh_find(fn);
    char d[192];
    snprintf(d, sizeof d, "0x%lx", a);
    CK(a != 0, "函数在数据表里", d);
    if (!a) return;

    // ---- 1. 用数据表反查 ----
    static const uint32_t CAND[] = { 16, 960, 100, 8, 4, 2, 1, 0, 3, 128 };
    P("  %s 的常量分布：", fn);
    for (unsigned i = 0; i < sizeof CAND/sizeof CAND[0]; ++i) {
        int c = zh_const_count(fn, CAND[i]);
        if (c > 0) P("    常量 %-6u 出现 %d 次", CAND[i], c);
    }

    // ★ 选"偏移最小"的那个：函数开头的常量必然每次调用都执行，
    //   末尾分支里的常量可能常年不跑（t13 第一次 hits=0 就是这个原因）。
    uint32_t pick = 0;
    unsigned long bestOff = ~0UL;
    for (unsigned i = 0; i < sizeof CAND/sizeof CAND[0]; ++i) {
        if (zh_const_count(fn, CAND[i]) <= 0) continue;
        for (int n = 0; n < 8; ++n) {
            unsigned long ca = 0;
            if (ii_const_addr(fn, CAND[i], n, &ca) != 0) break;
            if (ca < bestOff) { bestOff = ca; pick = CAND[i]; }
        }
    }
    snprintf(d, sizeof d, "value=%u @+0x%lx（最早执行的）",
             pick, bestOff > a ? bestOff - a : 0);
    CK(pick != 0, "选出最早执行的常量", d);
    if (!pick) { P("=== t13 完成（无可用常量）==="); return; }

    // ---- 2. 取该常量最早的那个调用点 ----
    unsigned long ca = 0;
    int bestN = 0;
    {
        unsigned long lo = ~0UL;
        for (int n = 0; n < 8; ++n) {
            unsigned long t2 = 0;
            if (ii_const_addr(fn, pick, n, &t2) != 0) break;
            if (t2 < lo) { lo = t2; bestN = n; }
        }
    }
    int rc = ii_const_addr(fn, pick, bestN, &ca);
    snprintf(d, sizeof d, "rc=%d addr=0x%lx", rc, ca);
    CK(rc == 0 && ca > a, "ii_const_addr 定位到指令", d);
    if (rc != 0 || !ca) return;

    snprintf(d, sizeof d, "函数+0x%lx", ca - a);
    CK(1, "常量指令相对函数偏移", d);

    // ---- 3. 插桩 ----
    int irc = ii_install_at_const(fn, pick, bestN, on_const, NULL);
    snprintf(d, sizeof d, "rc=%d (%s)", irc, ii_strerror(irc));
    CK(irc == 0, "ii_install_at_const 安装", d);
    if (irc != 0) { P("  提示: %s", ii_strhint(irc)); return; }

    // ---- 4. 观察 ----
    P("=== 观察 3 秒 ===");
    for (int i = 1; i <= 3; ++i) {
        sleep(1);
        P("  t=%ds  hits=%lu", i, __atomic_load_n(&g_hits, __ATOMIC_RELAXED));
    }
    unsigned long hits = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    snprintf(d, sizeof d, "%lu 次", hits);
    CK(hits > 0, "常量指令被命中", d);

    // ---- 5. 卸载 ----
    int un = ii_uninstall(ca);
    snprintf(d, sizeof d, "rc=%d", un);
    CK(un == 0, "ii_uninstall 卸载", d);

    int still = (ii_find(ca) != NULL);
    CK(!still, "站点已清除", still ? "仍在" : "已清除");

    P("=== t13 完成 ===");
}
