// 步骤 11：多实例（ii_engine_create）
//
// 验证：
//   1. 能创建独立实例
//   2. 实例互不干扰（同一地址，不同实例各自的站点/回调）
//   3. 销毁实例会卸载它的站点，不影响全局引擎
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
        g_fd = open("/data/data/com.netease.x19/cache/zh_t11.txt",
                    O_WRONLY|O_CREAT|O_APPEND, 0666);
        if (g_fd < 0) return;
    }
    char b[512]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++]='\n'; (void)!write(g_fd, b, n); }
}
static void CK(int c, const char* n, const char* d) {
    P("%s %-30s %s", c?"OK  ":"FAIL", n, d?d:"");
}

static volatile unsigned long g_a = 0, g_b = 0, g_g = 0;
static void cbA(ii_ctx* c, void* u) { (void)c;(void)u; __atomic_add_fetch(&g_a,1,__ATOMIC_RELAXED); }
static void cbB(ii_ctx* c, void* u) { (void)c;(void)u; __atomic_add_fetch(&g_b,1,__ATOMIC_RELAXED); }
static void cbG(ii_ctx* c, void* u) { (void)c;(void)u; __atomic_add_fetch(&g_g,1,__ATOMIC_RELAXED); }

static void* worker(void* arg) {
    (void)arg;
    P("");
    P("════════ 运行 #%ld ════════", (long)time(NULL));
    P("=== t11: 多实例 ===");

    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return NULL; }
    unsigned long base = zh_module_base();

    const char* fn = "LocalPlayer_normalTick";
    unsigned long entry = zh_find(fn);
    if (!entry) { P("FAIL 找不到 %s", fn); return NULL; }
    P("base=0x%lx  %s @ 0x%lx", base, fn, entry);

    char d[160];

    // ---- 1. 创建两个实例 ----
    ii_engine* A = ii_engine_create();
    ii_engine* B = ii_engine_create();
    snprintf(d, sizeof d, "A=%p B=%p", (void*)A, (void*)B);
    CK(A && B && A != B, "ii_engine_create ×2（互不相同）", d);
    if (!A || !B) return NULL;

    int ca = ii_engine_count(A), cbn = ii_engine_count(B);
    snprintf(d, sizeof d, "A=%d B=%d", ca, cbn);
    CK(ca == 0 && cbn == 0, "新实例初始为空", d);

    // ---- 2. 同一地址装到两个实例 ----
    int r1 = ii_engine_install(A, entry, cbA, NULL);
    snprintf(d, sizeof d, "rc=%d", r1);
    CK(r1 == 0, "实例A 装 hook", d);

    int r2 = ii_engine_install(B, entry, cbB, NULL);
    snprintf(d, sizeof d, "rc=%d", r2);
    CK(r2 == 0, "实例B 装同一地址的 hook", d);

    int ca2 = ii_engine_count(A), cb2 = ii_engine_count(B);
    snprintf(d, sizeof d, "A=%d B=%d", ca2, cb2);
    CK(ca2 == 1 && cb2 == 1, "两实例各 1 个站点", d);

    // ---- 3. 全局引擎挂到同一地址的链上（共享站点，这是正确语义）----
    int r3 = zh_hook(fn, cbG, NULL);
    snprintf(d, sizeof d, "rc=%d", r3);
    CK(r3 == 0, "全局引擎挂到同一站点", d);

    // 同一地址只有一个站点：A/B/全局 的回调都挂它上面
    int chain = ii_callback_count(entry);
    snprintf(d, sizeof d, "%d 个（A+B+全局）", chain);
    CK(chain == 3, "共享站点上有 3 个回调", d);

    // ---- 4. 观察：三个都应触发 ----
    P("=== 观察 2 秒 ===");
    __atomic_store_n(&g_a, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_b, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_g, 0, __ATOMIC_RELAXED);
    sleep(2);
    unsigned long va = __atomic_load_n(&g_a,__ATOMIC_RELAXED);
    unsigned long vb = __atomic_load_n(&g_b,__ATOMIC_RELAXED);
    unsigned long vg = __atomic_load_n(&g_g,__ATOMIC_RELAXED);
    P("  A=%lu  B=%lu  全局=%lu", va, vb, vg);

    snprintf(d, sizeof d, "%lu / %lu / %lu", va, vb, vg);
    CK(va > 0 && vb > 0 && vg > 0, "三路回调都触发", d);

    // ---- 5. 销毁 A，B 和全局应继续工作 ----
    ii_engine_destroy(A);
    P("=== 销毁实例A 后观察 2 秒 ===");
    __atomic_store_n(&g_a, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_b, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_g, 0, __ATOMIC_RELAXED);
    sleep(2);
    va = __atomic_load_n(&g_a,__ATOMIC_RELAXED);
    vb = __atomic_load_n(&g_b,__ATOMIC_RELAXED);
    vg = __atomic_load_n(&g_g,__ATOMIC_RELAXED);
    P("  A=%lu  B=%lu  全局=%lu", va, vb, vg);

    // ★ 重构后的语义：destroy(A) 只撤 A 自己那份回调，
    //   B 和全局引擎挂在同一站点上的回调应继续工作。
    snprintf(d, sizeof d, "%lu", va);
    CK(va == 0, "A 的回调已撤（不再触发）", d);
    snprintf(d, sizeof d, "%lu", vb);
    CK(vb > 0, "B 不受影响（仍在触发）", d);
    snprintf(d, sizeof d, "%lu", vg);
    CK(vg > 0, "全局引擎不受影响", d);
    int stillShared = (ii_find(entry) != NULL);
    snprintf(d, sizeof d, "站点%s", stillShared ? "仍在" : "已撤");
    CK(stillShared, "共享站点仍在（B/全局 还用着）", d);

    // ---- 6. 销毁 B，只留全局 ----
    P("  [step] 调用 destroy(B)");
    ii_engine_destroy(B);
    P("  [step] destroy(B) 返回");
    P("=== 销毁实例B 后观察 1 秒 ===");
    P("  [step] 即将 destroy(B)");
    __atomic_store_n(&g_b, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_g, 0, __ATOMIC_RELAXED);
    P("  [step] sleep(1) 前");
    sleep(1);
    P("  [step] sleep(1) 后");
    vb = __atomic_load_n(&g_b,__ATOMIC_RELAXED);
    vg = __atomic_load_n(&g_g,__ATOMIC_RELAXED);
    snprintf(d, sizeof d, "B=%lu 全局=%lu", vb, vg);
    CK(vb == 0 && vg > 0, "B 销毁，全局仍在", d);

    // ---- 7. 全部卸载 ----
    P("  [step] 即将 zh_unhook");
    int urc = zh_unhook(fn);
    P("  [step] zh_unhook 返回 rc=%d", urc);
    snprintf(d, sizeof d, "rc=%d", urc);
    CK(urc == 0, "zh_unhook 全局", d);

    P("=== t11 完成 ===");
    return NULL;
}

// ★ 必须在 constructor 里立刻返回（见 t08d 的说明）：
//   constructor 由 dlopen 同步执行，若在里面 sleep 会卡住注入线程，
//   若那正好是游戏主线程/渲染线程 → 触摸无响应 → ANR 闪退。
//   前 20 轮调试的崩溃全部是这个原因，不是引擎问题。
__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
