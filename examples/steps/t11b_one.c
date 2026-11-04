// t11b：只让实例 A 装一个 hook，然后销毁
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        g_fd = open("/data/data/com.netease.x19/cache/zh_t11.txt",
                    O_WRONLY|O_CREAT|O_APPEND, 0666);
        if (g_fd < 0) return;
    }
    char b[384]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++]='\n'; (void)!write(g_fd, b, n); }
}
static volatile unsigned long g_hits = 0;
static void cbA(ii_ctx* c, void* u) { (void)c;(void)u; __atomic_add_fetch(&g_hits,1,__ATOMIC_RELAXED); }

__attribute__((constructor))
static void run(void) {
    P("");
    P("════ t11b 单实例装一个 hook ════");
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return; }
    P("  >> 引擎就绪");

    unsigned long entry = zh_find("LocalPlayer_normalTick");
    P("  tick @ 0x%lx", entry);
    if (!entry) return;

    ii_engine* A = ii_engine_create();
    P("  create A = %p", (void*)A);
    if (!A) return;

    int rc = ii_engine_install(A, entry, cbA, NULL);
    P("  install rc=%d  count=%d", rc, ii_engine_count(A));
    P("  >> install 完成");

    sleep(2);
    P("  2 秒 hits=%lu", __atomic_load_n(&g_hits,__ATOMIC_RELAXED));
    P("  >> 观察完成");

    ii_engine_destroy(A);
    P("  >> destroy 完成 ← 没崩就说明单实例没问题");

    sleep(1);
    P("  destroy 后 hits=%lu", __atomic_load_n(&g_hits,__ATOMIC_RELAXED));
    P("════ t11b 结束 ════");
}
