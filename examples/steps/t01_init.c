// 步骤 1：只做初始化，验证引擎能否就绪
// 不做任何 hook，最安全
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        g_fd = open("/data/data/com.netease.x19/cache/zh_t01.txt",
                    O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (g_fd < 0) return;
    }
    char b[512]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++]='\n'; (void)!write(g_fd, b, n); }
}

__attribute__((constructor))
static void run(void) {
    P("=== t01: 只初始化 ===");

    zh_layer_cfg cfg = ZH_LAYER_CFG_DEFAULT;
    cfg.logPath  = "/data/data/com.netease.x19/cache/zh_t01_layer.log";
    cfg.logLevel = ZLOG_INFO;

    int rc = zh_layer_init(&cfg);
    P("zh_layer_init -> %d (0=OK 1=已初始化)", rc);

    if (rc != ZH_OK && rc != ZH_E_ALREADY) {
        P("FAIL 初始化失败 rc=%d", rc);
        return;
    }
    P("ready        = %d", zh_layer_ready());
    P("module_base  = 0x%lx", zh_module_base());
    P("game_version = 0x%llx", (unsigned long long)zh_game_version());

    char st[256];
    zh_layer_status(st, sizeof st);
    P("status: %s", st);

    char chk[1024];
    int probs = zh_selfcheck(chk, sizeof chk);
    P("selfcheck: %d 个问题", probs);
    P("%s", chk);

    P("=== t01 完成（不做任何 hook）===");
}
