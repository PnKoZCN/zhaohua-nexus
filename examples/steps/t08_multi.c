// 步骤 8：多回调链（对标 Mixin 多个 @Inject 挂同一方法）
//
// 在同一个地址上挂 3 个回调，不同 priority，验证：
//   1. 三个都被调用
//   2. 按 priority 降序
//   3. 卸载全部成功
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        g_fd = open("/data/data/com.netease.x19/cache/zh_t08.txt",
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

static volatile int g_order[8];
static volatile int g_n = 0;

static void cb_hi (ii_ctx* c, void* u) { (void)c;(void)u;
    int i = __atomic_fetch_add(&g_n, 1, __ATOMIC_RELAXED); if(i<8) g_order[i]=300; }
static void cb_mid(ii_ctx* c, void* u) { (void)c;(void)u;
    // 注意：zh_hook 用 II_PRIO_DEFAULT = 0，所以记录 0
    int i = __atomic_fetch_add(&g_n, 1, __ATOMIC_RELAXED); if(i<8) g_order[i]=0; }
static void cb_lo (ii_ctx* c, void* u) { (void)c;(void)u;
    int i = __atomic_fetch_add(&g_n, 1, __ATOMIC_RELAXED); if(i<8) g_order[i]=100; }

__attribute__((constructor))
static void run(void) {
    P("=== t08: 多回调链 ===");

    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return; }
    unsigned long base = zh_module_base();

    const char* fn = "LocalPlayer_normalTick";
    unsigned long entry = zh_find(fn);
    if (!entry) { P("FAIL 找不到 %s", fn); return; }
    P("base=0x%lx  %s @ 0x%lx", base, fn, entry);

    // ---- 1. 第一个回调（zh_hook 建站点）----
    int rc = zh_hook(fn, cb_mid, NULL);
    char d[96]; snprintf(d, sizeof d, "rc=%d", rc);
    CK(rc == 0, "zh_hook (priority 默认)", d);
    if (rc != 0) return;

    // ---- 2. 追加第二个（高优先级）----
    int rc2 = ii_add_callback(entry, cb_hi, NULL, 300);
    snprintf(d, sizeof d, "rc=%d", rc2);
    CK(rc2 == 0, "ii_add_callback (prio 300)", d);

    // ---- 3. 追加第三个（低优先级）----
    int rc3 = ii_add_callback(entry, cb_lo, NULL, 100);
    snprintf(d, sizeof d, "rc=%d", rc3);
    CK(rc3 == 0, "ii_add_callback (prio 100)", d);

    // ---- 4. 回调数 ----
    int ncb = ii_callback_count(entry);
    snprintf(d, sizeof d, "%d 个", ncb);
    CK(ncb == 3, "ii_callback_count", d);

    // ---- 5. 观察执行顺序 ----
    P("=== 观察 1 秒 ===");
    __atomic_store_n(&g_n, 0, __ATOMIC_RELAXED);
    sleep(1);
    int cnt = __atomic_load_n(&g_n, __ATOMIC_RELAXED);
    snprintf(d, sizeof d, "%d 次调用", cnt);
    CK(cnt > 0, "回调被触发", d);

    P("  前 3 次的执行顺序: %d %d %d", g_order[0], g_order[1], g_order[2]);
    // zh_hook 用 II_PRIO_DEFAULT=0，故实际优先级是 300 / 100 / 0
    CK(g_order[0] == 300 && g_order[1] == 100 && g_order[2] == 0,
       "按 priority 降序 (300→100→0)", "");

    // ---- 6. 卸载单个回调 ----
    int rcr = ii_remove_callback(entry, cb_hi, NULL);
    snprintf(d, sizeof d, "rc=%d", rcr);
    CK(rcr == 0, "ii_remove_callback (移除 prio300)", d);
    int ncb2 = ii_callback_count(entry);
    snprintf(d, sizeof d, "%d 个", ncb2);
    CK(ncb2 == 2, "移除后剩 2 个", d);

    // ---- 7. 全长卸载 ----
    int urc = zh_unhook(fn);
    snprintf(d, sizeof d, "rc=%d", urc);
    CK(urc == 0, "zh_unhook", d);

    int ncb3 = ii_callback_count(entry);
    snprintf(d, sizeof d, "%d", ncb3);
    CK(ncb3 == 0, "卸载后回调数归零", d);

    P("=== t08 完成 ===");
}
