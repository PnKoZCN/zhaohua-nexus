// mla：多库共存测试 —— 库 A（建立站点者）
//
// 流程：
//   1. 初始化，设置 owner = "modA"
//   2. hook LocalPlayer_normalTick（此时是【建立站点】）
//   3. 计数 25 秒
//   4. 主动 zh_unhook_owner("modA") —— 触发 site_release 的转交路径
//      （如果库 B 已挂到同一站点，入口必须保留、shell 不能归还）
//   5. 记录卸载结果，继续活着观察 10 秒
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
        g_fd = open("/data/data/com.netease.x19/cache/zh_mlA.txt",
                    O_WRONLY|O_CREAT|O_APPEND, 0666);
        if (g_fd < 0) return;
    }
    char b[320]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++] = '\n'; (void)!write(g_fd, b, n); }
}

static volatile unsigned long g_n = 0;
static void cbA(ii_ctx* c, void* u) {
    (void)c; (void)u;
    __atomic_add_fetch(&g_n, 1, __ATOMIC_RELAXED);
}

static void* worker(void* arg) {
    (void)arg;
    const char* FN = "LocalPlayer_normalTick";

    P("");
    P("════ 库 A 启动 ════");
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) {
        P("FAIL 引擎初始化"); return NULL;
    }

    unsigned long addr = zh_find(FN);
    P("  %s @0x%lx", FN, addr);
    if (!addr) { P("FAIL 找不到函数"); return NULL; }

    int r = zh_hook_owned(FN, cbA, NULL, "modA");
    P("  A zh_hook rc=%d 链长=%d", r, ii_callback_count(addr));

    for (int i = 0; i < 5; ++i) {
        sleep(5);
        P("  A t=%2ds n=%lu", (i+1)*5, __atomic_load_n(&g_n, __ATOMIC_RELAXED));
    }

    // ★ 关键：主动卸载自己的 owner，触发转交路径
    unsigned long before = __atomic_load_n(&g_n, __ATOMIC_RELAXED);
    P("  A 卸载前计数=%lu，调用 zh_unhook_owner(\"modA\")", before);

    int n = zh_unhook_owner("modA");
    P("  A zh_unhook_owner 返回 %d（1=站点已卸载，0=仅摘回调或未匹配）", n);

    // 看入口是否被保留（站点数不减说明转交了）
    P("  A 卸载后 ii_count()=%d 链长=%d",
      ii_count(), ii_callback_count(addr));

    // 再观察 10 秒：如果 B 还在用，B 的计数应该继续涨
    for (int i = 0; i < 2; ++i) {
        sleep(5);
        P("  A 卸载后 +%ds  A计数=%lu（应停止增长；若仍增长说明归属匹配失效）",
          (i+1)*5, __atomic_load_n(&g_n, __ATOMIC_RELAXED));
    }

    P("════ 库 A 结束 ════");
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
