// 步骤 5b：在高频函数上验证返回点 hook
//
// LocalPlayer_normalTick 每 tick 被调用一次（20 次/秒，t04 已证实）。
// 装它的所有返回点，确认回调真的会被触发。
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        g_fd = open("/data/data/com.netease.x19/cache/zh_t05b.txt",
                    O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (g_fd < 0) return;
    }
    char b[384]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++]='\n'; (void)!write(g_fd, b, n); }
}
static void CK(int c, const char* n, const char* d) {
    P("%s %-28s %s", c?"OK  ":"FAIL", n, d?d:"");
}

static volatile unsigned long g_hits = 0;
static volatile int  g_hitMask = 0;      // 哪些返回点被命中（位图）

// 单个回调，靠 user 传返回点序号
static void on_ret(ii_ctx* c, void* user) {
    (void)c;
    int idx = (int)(uintptr_t)user;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);
    if (idx >= 0 && idx < 32)
        __atomic_fetch_or(&g_hitMask, 1u << idx, __ATOMIC_RELAXED);
}

__attribute__((constructor))
static void run(void) {
    P("=== t05b: 高频函数的返回点 hook ===");

    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return; }
    unsigned long base = zh_module_base();

    const char* fn = "LocalPlayer_normalTick";
    unsigned long entry = zh_find(fn);
    if (!entry) { P("FAIL 找不到 %s", fn); return; }
    P("base=0x%lx  目标 %s @ 0x%lx (base+0x%lx)", base, fn, entry, entry-base);

    int nret = ii_retpoint_count(fn);
    char d[96]; snprintf(d, sizeof d, "%d 个", nret);
    CK(nret > 0, "ii_retpoint_count", d);
    if (nret <= 0) return;
    if (nret > 32) { P("返回点过多(%d)，只装前 32 个", nret); nret = 32; }

    int installed = 0;
    for (int i = 0; i < nret; ++i) {
        unsigned long ra = 0;
        if (ii_retpoint_addr(fn, i, &ra) != 0 || !ra) continue;
        int rc = ii_install(ra - 4, on_ret, (void*)(uintptr_t)i);
        if (rc == 0) installed++;
        else P("  ret[%2d] @0x%lx rc=%d (%s)", i, ra-4, rc, ii_strerror(rc));
    }
    snprintf(d, sizeof d, "%d/%d", installed, nret);
    CK(installed > 0, "安装返回点", d);

    P("=== 观察 3 秒（预期约 20 次/秒）===");
    for (int i = 1; i <= 3; ++i) {
        sleep(1);
        P("  t=%ds  hits=%lu  mask=0x%x",
          i, __atomic_load_n(&g_hits, __ATOMIC_RELAXED),
          __atomic_load_n(&g_hitMask, __ATOMIC_RELAXED));
    }

    unsigned long hits = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    snprintf(d, sizeof d, "%lu 次", hits);
    CK(hits > 0, "回调被触发", d);

    if (hits > 0) {
        snprintf(d, sizeof d, "约 %.0f 次/秒", hits/3.0);
        CK(1, "调用频率", d);
        int mask = __atomic_load_n(&g_hitMask, __ATOMIC_RELAXED);
        snprintf(d, sizeof d, "0x%x", mask);
        CK(mask != 0, "命中的返回点", d);
    }

    // ---- 卸载 ----
    int rc = zh_unhook(fn);
    snprintf(d, sizeof d, "rc=%d", rc);
    CK(rc == 0, "zh_unhook", d);

    // ---- 验证全部恢复 ----
    int restored = 0, still = 0;
    for (int i = 0; i < nret; ++i) {
        unsigned long ra = 0;
        if (ii_retpoint_addr(fn, i, &ra) != 0 || !ra) continue;
        unsigned int w = *(volatile unsigned int*)(ra - 4);
        if ((w & 0xFC000000u) == 0x14000000u) still++;
        else restored++;
    }
    snprintf(d, sizeof d, "恢复 %d，残留 %d", restored, still);
    CK(still == 0, "全部返回点已恢复", d);

    // ---- 卸载后不再增长 ----
    unsigned long at = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    sleep(1);
    unsigned long after = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    snprintf(d, sizeof d, "%lu -> %lu", at, after);
    CK(after == at, "卸载后不再触发", d);

    P("=== t05b 完成 ===");
}
