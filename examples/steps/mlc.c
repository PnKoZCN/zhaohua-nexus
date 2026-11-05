// mlc：收尾验证 —— 最后一个归属者卸载时，入口指令是否被还原
//
// 承接 A/B：B（modB）还在站点上。这里
//   1. 记录当前入口指令（应是 b→shell，被补丁状态）
//   2. 取站点里引擎记录的原始指令 orig
//   3. zh_unhook_owner("modB")
//   4. 再读入口指令，应该 == orig（还原成功）
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
        g_fd = open("/data/data/com.netease.x19/cache/zh_mlC.txt",
                    O_WRONLY|O_CREAT|O_APPEND, 0666);
        if (g_fd < 0) return;
    }
    char b[320]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++] = '\n'; (void)!write(g_fd, b, n); }
}

static void* worker(void* arg) {
    (void)arg;
    const char* FN = "LocalPlayer_normalTick";

    P("");
    P("════ 收尾验证 ════");
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) {
        P("  FAIL 引擎初始化"); return NULL;
    }

    unsigned long a = zh_find(FN);
    P("  %s @0x%lx", FN, a);
    if (!a) return NULL;

    ii_site* s = ii_find(a);
    if (!s) {
        P("  本地查不到站点（B 的站点在共享引擎里，说明已卸载或归属不同实例）");
        P("  当前入口指令 = 0x%08x", *(volatile uint32_t*)a);
        P("════ 结束 ════");
        return NULL;
    }

    uint32_t orig0 = s->orig;
    uint32_t live0 = *(volatile uint32_t*)a;
    P("  卸载前：live=0x%08x orig=0x%08x %s",
      live0, orig0, (live0 != orig0) ? "（处于补丁状态）" : "（已是原指令）");

    int n = zh_unhook_owner("modB");
    P("  zh_unhook_owner(\"modB\") 返回 %d（1=站点已卸载）", n);

    // 等一拍，确保写回完成
    sleep(1);
    uint32_t live1 = *(volatile uint32_t*)a;
    P("  卸载后：live=0x%08x orig=0x%08x → %s",
      live1, orig0,
      (live1 == orig0) ? "★入口已还原" : "✗入口未还原！");

    P("  ii_count=%d 本地站点=%s", ii_count(), ii_find(a) ? "仍在" : "已移除");
    P("════ 结束 ════");
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
