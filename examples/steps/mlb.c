// mlb：多库共存测试 —— 库 B（后来者，挂到已有站点）
//
// 流程：
//   1. 初始化，设置 owner = "modB"
//   2. hook 同一函数 —— 此时应走【共享站点 + 追加回调】路径
//   3. 持续计数 45 秒，跨过 A 的卸载时刻
//   4. 验证 A 卸载后自己仍继续工作
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        g_fd = open("/data/data/com.netease.x19/cache/zh_mlB.txt",
                    O_WRONLY|O_CREAT|O_APPEND, 0666);
        if (g_fd < 0) return;
    }
    char b[320]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++] = '\n'; (void)!write(g_fd, b, n); }
}

static volatile unsigned long g_n = 0;
static void cbB(ii_ctx* c, void* u) {
    (void)c; (void)u;
    __atomic_add_fetch(&g_n, 1, __ATOMIC_RELAXED);
}

static void* worker(void* arg) {
    (void)arg;
    const char* FN = "LocalPlayer_normalTick";

    P("");
    P("════ 库 B 启动 ════");
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) {
        P("FAIL 引擎初始化"); return NULL;
    }

    unsigned long addr = zh_find(FN);
    if (!addr) { P("FAIL 找不到函数"); return NULL; }

    int r = zh_hook_owned(FN, cbB, NULL, "modB");
    P("  B zh_hook rc=%d（0=挂到共享站点，-2=已有本地站点）", r);
    P("  B 链长=%d ii_count=%d", ii_callback_count(addr), ii_count());

    // 45 秒，跨过 A 在 t=25s 的卸载
    for (int i = 0; i < 9; ++i) {
        sleep(5);
        P("  B t=%2ds n=%lu  ii_count=%d", (i+1)*5,
          __atomic_load_n(&g_n, __ATOMIC_RELAXED), ii_count());
    }

    P("════ 库 B 结束（n=%lu）════",
      __atomic_load_n(&g_n, __ATOMIC_RELAXED));
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
