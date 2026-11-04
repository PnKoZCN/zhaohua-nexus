// t11a：最小复现 —— 只创建并销毁空实例，不装任何 hook
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
#define STEP(x) do { P("  >> " x); } while(0)

__attribute__((constructor))
static void run(void) {
    P("");
    P("════ t11a 最小复现 ════");
    STEP("开始");
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return; }
    STEP("引擎就绪");

    ii_engine* A = ii_engine_create();
    P("  create A = %p", (void*)A);
    if (!A) return;
    STEP("create 完成");

    P("  A->nSites 应为 0，count=%d", ii_engine_count(A));
    STEP("count 完成");

    ii_engine_destroy(A);
    STEP("destroy 完成 ← 若能打出来，说明 destroy 本身不崩");

    P("════ t11a 结束 ════");
}
