// 步骤 5c：诊断返回点为什么不触发
//
// 对照实验：
//   A. 入口 hook（t04 已证明 20 次/秒）
//   B. 返回点 hook
// 如果 A 有、B 没有 → 执行流真的没到那些 ret
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        g_fd = open("/data/data/com.netease.x19/cache/zh_t05c.txt",
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

static volatile unsigned long g_entryHits = 0;
static volatile unsigned long g_retHits = 0;
static volatile int g_retMask = 0;

static void on_entry(ii_ctx* c, void* u) { (void)c;(void)u;
    __atomic_add_fetch(&g_entryHits, 1, __ATOMIC_RELAXED); }

static void on_ret(ii_ctx* c, void* u) { (void)c;
    __atomic_add_fetch(&g_retHits, 1, __ATOMIC_RELAXED);
    int idx=(int)(uintptr_t)u;
    if (idx>=0 && idx<32) __atomic_fetch_or(&g_retMask, 1u<<idx, __ATOMIC_RELAXED);
}

__attribute__((constructor))
static void run(void) {
    P("=== t05c: 入口 vs 返回点 对照 ===");

    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return; }
    unsigned long base = zh_module_base();

    const char* fn = "LocalPlayer_normalTick";
    unsigned long entry = zh_find(fn);
    if (!entry) { P("FAIL 找不到"); return; }
    P("base=0x%lx entry=0x%lx (base+0x%lx)", base, entry, entry-base);

    // ---- A: 入口 hook（FULL，能同时读 x0 和上下文）----
    int rcA = zh_hook(fn, on_entry, NULL);
    char d[96]; snprintf(d, sizeof d, "rc=%d", rcA);
    CK(rcA == 0, "A. 入口 hook (FULL)", d);

    // ---- B: 返回点 hook ----
    int nret = ii_retpoint_count(fn);
    int inst = 0;
    for (int i = 0; i < nret && i < 32; ++i) {
        unsigned long ra = 0;
        if (ii_retpoint_addr(fn, i, &ra) != 0 || !ra) continue;
        // 先检查能否插桩
        int can = ii_can_instrument(ra - 4);
        if (!can) {
            unsigned int w = *(volatile unsigned int*)(ra - 4);
            P("  ret[%2d] +0x%lx 不可插桩  前指令=0x%08x",
              i, ra-entry-4, w);
            continue;
        }
        if (ii_install(ra - 4, on_ret, (void*)(uintptr_t)i) == 0) inst++;
    }
    snprintf(d, sizeof d, "%d/%d", inst, nret);
    CK(inst > 0, "B. 返回点 hook", d);

    // ---- 观察 ----
    P("=== 观察 3 秒 ===");
    for (int i = 1; i <= 3; ++i) {
        sleep(1);
        P("  t=%ds  entry=%lu  ret=%lu  mask=0x%x", i,
          __atomic_load_n(&g_entryHits, __ATOMIC_RELAXED),
          __atomic_load_n(&g_retHits, __ATOMIC_RELAXED),
          __atomic_load_n(&g_retMask, __ATOMIC_RELAXED));
    }

    unsigned long eh = __atomic_load_n(&g_entryHits, __ATOMIC_RELAXED);
    unsigned long rh = __atomic_load_n(&g_retHits, __ATOMIC_RELAXED);

    snprintf(d, sizeof d, "%lu 次", eh); CK(eh > 0, "入口触发", d);
    snprintf(d, sizeof d, "%lu 次", rh); CK(rh > 0, "返回点触发", d);

    if (eh > 0 && rh == 0) {
        P("");
        P("★ 结论：入口有 %lu 次，返回点 0 次", eh);
        P("  → 执行流没有经过任何被改写的返回点");
        P("  → 说明 normalTick 在这条路径上走的是【未被收录的 ret】");
        P("  → 或者 20 个返回点都是条件分支里的罕见路径");
    } else if (eh > 0 && rh > 0) {
        P("★ 结论：入口 %lu 次 / 返回点 %lu 次（比例 %.0f%%）",
          eh, rh, rh*100.0/eh);
    }

    // ---- 卸载 ----
    int rc = zh_unhook(fn);
    snprintf(d, sizeof d, "rc=%d", rc); CK(rc == 0, "卸载", d);

    P("=== t05c 完成 ===");
}
