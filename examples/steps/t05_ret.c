// 步骤 5：返回点 hook（zh_hook_ret）
//
// 改进：装全部返回点（不是只装 TAIL），确保能命中。
// 同时验证修复后的 zh_unhook 能正确卸载所有返回点。
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
        g_fd = open("/data/data/com.netease.x19/cache/zh_t05.txt",
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
static volatile float         g_lastHp = -1.0f;
static volatile int           g_retIdx = -1;

// 返回点回调：读 float 返回值 + 记录命中的返回点序号
static void on_health(void* a, void* b) {
    // 用可变参数签名统一（user 里存返回点序号）
    ii_ctx* c = (ii_ctx*)a;
    int idx = (int)(uintptr_t)b;
    (void)idx;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);
    union { __uint128_t q; float f; } u;
    u.q = c->q[0];
    g_lastHp = u.f;
}

// 每个返回点用独立回调（user 存序号）
#define MAKE_CB(n) \
    static void cb_ret_##n(ii_ctx* c, void* u) { \
        (void)u; __atomic_add_fetch(&g_hits,1,__ATOMIC_RELAXED); \
        union { __uint128_t q; float f; } x; x.q = c->q[0]; \
        g_lastHp = x.f; g_retIdx = n; }
MAKE_CB(0)  MAKE_CB(1)  MAKE_CB(2)  MAKE_CB(3)
MAKE_CB(4)  MAKE_CB(5)  MAKE_CB(6)  MAKE_CB(7)
MAKE_CB(8)  MAKE_CB(9)  MAKE_CB(10) MAKE_CB(11)
MAKE_CB(12) MAKE_CB(13) MAKE_CB(14) MAKE_CB(15)
MAKE_CB(16) MAKE_CB(17) MAKE_CB(18) MAKE_CB(19)
MAKE_CB(20) MAKE_CB(21) MAKE_CB(22) MAKE_CB(23)
MAKE_CB(24) MAKE_CB(25) MAKE_CB(26) MAKE_CB(27)

static ii_callback g_cbs[28] = {
    cb_ret_0, cb_ret_1, cb_ret_2, cb_ret_3, cb_ret_4, cb_ret_5, cb_ret_6, cb_ret_7,
    cb_ret_8, cb_ret_9, cb_ret_10, cb_ret_11, cb_ret_12, cb_ret_13, cb_ret_14, cb_ret_15,
    cb_ret_16, cb_ret_17, cb_ret_18, cb_ret_19, cb_ret_20, cb_ret_21, cb_ret_22, cb_ret_23,
    cb_ret_24, cb_ret_25, cb_ret_26, cb_ret_27,
};

__attribute__((constructor))
static void run(void) {
    P("=== t05: 返回点 hook（全部 28 个）===");

    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return; }
    unsigned long base = zh_module_base();
    P("base = 0x%lx", base);

    const char* fn = "Actor_getHealth";
    unsigned long entry = zh_find(fn);
    if (!entry) { P("FAIL 找不到 %s", fn); return; }
    P("目标 %s @ 0x%lx (base+0x%lx)", fn, entry, entry-base);

    int nret = ii_retpoint_count(fn);
    char d[96]; snprintf(d, sizeof d, "%d 个", nret);
    CK(nret > 0, "ii_retpoint_count", d);
    if (nret <= 0 || nret > 28) return;

    // ---- 逐条安装全部返回点 ----
    int installed = 0;
    for (int i = 0; i < nret; ++i) {
        unsigned long ra = 0;
        if (ii_retpoint_addr(fn, i, &ra) != 0 || !ra) continue;
        unsigned int orig = *(volatile unsigned int*)(ra - 4);
        int rc = ii_install(ra - 4, g_cbs[i], NULL);
        if (rc == 0) {
            unsigned int aft = *(volatile unsigned int*)(ra - 4);
            int isb = (aft & 0xFC000000u) == 0x14000000u;
            P("  ret[%2d] @0x%lx  0x%08x -> 0x%08x  rc=%d %s",
              i, ra-4, orig, aft, rc, isb ? "(b)" : "(!!)");
            installed++;
        } else {
            P("  ret[%2d] @0x%lx  rc=%d (%s)", i, ra-4, rc, ii_strerror(rc));
        }
    }
    snprintf(d, sizeof d, "%d/%d 个", installed, nret);
    CK(installed > 0, "安装返回点", d);

    // ---- 观察 ----
    P("=== 观察 3 秒 ===");
    for (int i = 1; i <= 3; ++i) {
        sleep(1);
        P("  t=%ds  hits=%lu  hp=%.1f  lastRet=%d",
          i, __atomic_load_n(&g_hits, __ATOMIC_RELAXED), g_lastHp, g_retIdx);
    }

    unsigned long hits = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    snprintf(d, sizeof d, "%lu 次", hits);
    CK(hits > 0, "回调被触发", d);
    if (hits > 0) {
        snprintf(d, sizeof d, "hp=%.1f  命中返回点=%d", g_lastHp, g_retIdx);
        CK(1, "读到返回值", d);
    }

    // ---- 卸载（测试修复后的 zh_unhook）----
    int rc = zh_unhook(fn);
    snprintf(d, sizeof d, "rc=%d", rc);
    CK(rc == 0, "zh_unhook（修复后）", d);

    // ---- 验证全部恢复 ----
    int restored = 0, still = 0;
    for (int i = 0; i < nret; ++i) {
        unsigned long ra = 0;
        if (ii_retpoint_addr(fn, i, &ra) != 0 || !ra) continue;
        unsigned int w = *(volatile unsigned int*)(ra - 4);
        if ((w & 0xFC000000u) == 0x14000000u) still++;
        else restored++;
    }
    snprintf(d, sizeof d, "恢复 %d 个，仍被改 %d 个", restored, still);
    CK(still == 0, "全部返回点已恢复", d);

    P("=== t05 完成 ===");
}
