// 步骤 3：装第一个 hook（只计数，不改任何东西）
//
// 选 Block_isAir：碰撞检测时调用，频率低，回调空转最安全。
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
        g_fd = open("/data/data/com.netease.x19/cache/zh_t03.txt",
                    O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (g_fd < 0) return;
    }
    char b[512]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++]='\n'; (void)!write(g_fd, b, n); }
}
static void CK(int c, const char* n, const char* d) {
    P("%s %-32s %s", c?"OK  ":"FAIL", n, d?d:"");
}

// ★ 回调：什么都不做，只计数
static volatile unsigned long g_hits = 0;
static void on_isair(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);
}

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec*1000.0 + t.tv_nsec/1e6;
}

__attribute__((constructor))
static void run(void) {
    P("=== t03: 装一个 hook（只计数）===");

    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return; }
    unsigned long base = zh_module_base();
    P("base = 0x%lx", base);

    const char* fn = "Block_isAir";
    unsigned long addr = zh_find(fn);
    if (!addr) { P("FAIL 找不到 %s", fn); return; }
    P("目标 %s @ 0x%lx (base+0x%lx)", fn, addr, addr - base);

    // ---- 1. 预检：能否插桩 ----
    int can = ii_can_instrument(addr);
    char d[64]; snprintf(d, sizeof d, "可以=%d", can);
    CK(can == 1, "ii_can_instrument 预检", d);
    if (!can) { P("FAIL 不可插桩，终止"); return; }

    // ---- 2. 读原始指令（备份）----
    unsigned int orig = *(volatile unsigned int*)addr;
    char d2[64]; snprintf(d2, sizeof d2, "0x%08x", orig);
    CK(1, "原始指令", d2);

    // ---- 3. 安装 hook ----
    int rc = zh_hook(fn, on_isair, NULL);
    char d3[128]; snprintf(d3, sizeof d3, "rc=%d (%s)", rc, ii_strerror(rc));
    CK(rc == 0, "zh_hook 安装", d3);
    if (rc != 0) {
        P("  建议: %s", ii_strhint(rc));
        return;
    }

    // ---- 4. 确认指令被改 ----
    unsigned int after = *(volatile unsigned int*)addr;
    int is_b = (after & 0xFC000000u) == 0x14000000u;
    char d4[80]; snprintf(d4, sizeof d4, "0x%08x → 0x%08x (b=%d)", orig, after, is_b);
    CK(is_b, "入口已改为 b 跳转", d4);

    // ---- 5. hook 计数 ----
    char d5[32]; snprintf(d5, sizeof d5, "%d", zh_active_count());
    CK(zh_active_count() == 1, "active_count", d5);

    P("=== 已安装，观察 3 秒 ===");

    // ---- 6. 观察命中 ----
    for (int i = 0; i < 3; ++i) {
        sleep(1);
        unsigned long h = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
        P("  t=%ds  hits=%lu", i+1, h);
    }

    unsigned long hits = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    char d6[64]; snprintf(d6, sizeof d6, "%lu 次", hits);
    CK(hits > 0, "回调被触发", d6);

    // ---- 7. 卸载 ----
    int urc = zh_unhook(fn);
    char d7[64]; snprintf(d7, sizeof d7, "rc=%d", urc);
    CK(urc == 0, "zh_unhook", d7);

    unsigned int restored = *(volatile unsigned int*)addr;
    char d8[80]; snprintf(d8, sizeof d8, "0x%08x (原值 0x%08x)", restored, orig);
    CK(restored == orig, "指令已恢复", d8);

    P("=== t03 完成 ===");
}
