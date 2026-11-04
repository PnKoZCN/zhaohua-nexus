// 步骤 4：验证回调真的被触发
//
// 用 LocalPlayer_normalTick（每帧调用）+ LIGHT 模式（省 88% 开销）
// 只计数，3 秒后立即卸载。
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
        g_fd = open("/data/data/com.netease.x19/cache/zh_t04.txt",
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
static volatile unsigned int  g_lastX0 = 0;

// ★ LIGHT 回调：只能读 x0-x7 / q0-q1
static void on_tick_light(ii_ctx_light* c, void* user) {
    (void)user;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);
    g_lastX0 = (unsigned int)(c->x[0] & 0xFFFFFFFFu);
}

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec*1000.0 + t.tv_nsec/1e6;
}

__attribute__((constructor))
static void run(void) {
    P("=== t04: 验证回调触发 ===");

    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return; }
    unsigned long base = zh_module_base();
    P("base = 0x%lx", base);

    const char* fn = "LocalPlayer_normalTick";
    unsigned long addr = zh_find(fn);
    if (!addr) { P("FAIL 找不到 %s", fn); return; }
    P("目标 %s @ 0x%lx (base+0x%lx)", fn, addr, addr-base);

    char d[64];
    snprintf(d, sizeof d, "可以=%d", ii_can_instrument(addr));
    CK(ii_can_instrument(addr) == 1, "预检", d);

    unsigned int orig = *(volatile unsigned int*)addr;
    snprintf(d, sizeof d, "0x%08x", orig);
    CK(1, "原始指令", d);

    // ---- LIGHT 模式安装 ----
    int rc = zh_hook_light(fn, on_tick_light, NULL);
    snprintf(d, sizeof d, "rc=%d (%s)", rc, ii_strerror(rc));
    CK(rc == 0, "zh_hook_light 安装", d);
    if (rc != 0) { P("  建议: %s", ii_strhint(rc)); return; }

    unsigned int after = *(volatile unsigned int*)addr;
    int is_b = (after & 0xFC000000u) == 0x14000000u;
    snprintf(d, sizeof d, "0x%08x → 0x%08x", orig, after);
    CK(is_b, "入口已改写", d);

    // ---- 观察 3 秒 ----
    P("=== 观察 3 秒（每帧调用）===");
    unsigned long prev = 0;
    for (int i = 1; i <= 3; ++i) {
        sleep(1);
        unsigned long h = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
        P("  t=%ds  hits=%lu  (+%lu)  x0low=0x%x", i, h, h-prev, g_lastX0);
        prev = h;
    }

    unsigned long hits = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    snprintf(d, sizeof d, "%lu 次", hits);
    CK(hits > 0, "回调被触发", d);

    // ---- 帧率估算 ----
    if (hits > 0) {
        snprintf(d, sizeof d, "约 %.0f 次/秒", hits/3.0);
        CK(1, "调用频率", d);
    }

    // ---- 卸载 ----
    rc = zh_unhook(fn);
    snprintf(d, sizeof d, "rc=%d", rc);
    CK(rc == 0, "zh_unhook", d);

    unsigned int restored = *(volatile unsigned int*)addr;
    snprintf(d, sizeof d, "0x%08x (原 0x%08x)", restored, orig);
    CK(restored == orig, "指令已恢复", d);

    // ---- 卸载后计数不再增长 ----
    unsigned long atUnhook = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    sleep(1);
    unsigned long after1s = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    snprintf(d, sizeof d, "%lu → %lu", atUnhook, after1s);
    CK(after1s == atUnhook, "卸载后不再触发", d);

    P("=== t04 完成 ===");
}
