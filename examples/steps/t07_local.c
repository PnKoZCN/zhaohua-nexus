// 步骤 7：栈局部变量（@Local 的部分替代）
//
// 在函数入口 hook，此时 sp 指向函数栈帧，可读 [sp + offset]。
// 目标 ItemStack_isNull（18 个栈槽记录，frameSize=0x440）
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        g_fd = open("/data/data/com.netease.x19/cache/zh_t07.txt",
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

static volatile unsigned long g_hits = 0;
// 记录几个槽位的读取结果
static volatile unsigned long g_slotVal[4] = {0,0,0,0};
static volatile int g_slotOk[4] = {0,0,0,0};
static uint16_t g_slotOff[4];
static uint8_t  g_slotSize[4];

static void on_entry(ii_ctx* c, void* user) {
    (void)user;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);
    // 只在第一次采样，避免热路径开销
    static int once = 0;
    if (__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED) != 0) return;
    for (int i = 0; i < 4; ++i) {
        if (!g_slotSize[i]) continue;
        int64_t v = zh_local_get(c, g_slotOff[i], g_slotSize[i]);
        g_slotVal[i] = (unsigned long)v;
        g_slotOk[i] = 1;
    }
}

__attribute__((constructor))
static void run(void) {
    P("=== t07: 栈局部变量 ===");

    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return; }
    unsigned long base = zh_module_base();

    const char* fn = "ItemStack_isNull";
    unsigned long entry = zh_find(fn);
    if (!entry) { P("FAIL 找不到 %s", fn); return; }
    P("base=0x%lx  目标 %s @ 0x%lx (base+0x%lx)", base, fn, entry, entry-base);

    // ---- 1. 栈帧信息 ----
    int fs = ii_frame_size(fn);
    char d[96]; snprintf(d, sizeof d, "0x%x (%d 字节)", fs, fs);
    CK(fs > 0, "ii_frame_size", d);

    int nslot = ii_local_count(fn);
    snprintf(d, sizeof d, "%d 个", nslot);
    CK(nslot > 0, "ii_local_count", d);

    // ---- 2. 枚举槽位 ----
    int pick = 0;
    for (int i = 0; i < nslot && pick < 4; ++i) {
        uint16_t so; uint8_t ss; uint32_t io;
        if (ii_local_slot(fn, i, &so, &ss, &io) != 0) continue;
        g_slotOff[pick] = so;
        g_slotSize[pick] = ss;
        P("  槽[%2d] sp+0x%-4x %d 字节  (首写 @+0x%x)", i, so, ss, io);
        pick++;
    }
    snprintf(d, sizeof d, "取了 %d 个", pick);
    CK(pick > 0, "ii_local_slot 枚举", d);
    if (pick == 0) return;

    // ---- 3. 安装入口 hook ----
    int rc = zh_hook(fn, on_entry, NULL);
    snprintf(d, sizeof d, "rc=%d", rc);
    CK(rc == 0, "zh_hook", d);
    if (rc != 0) return;

    // ---- 4. 观察 ----
    P("=== 观察 3 秒 ===");
    for (int i = 1; i <= 3; ++i) {
        sleep(1);
        P("  t=%ds  hits=%lu", i, __atomic_load_n(&g_hits, __ATOMIC_RELAXED));
    }

    unsigned long hits = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    snprintf(d, sizeof d, "%lu 次", hits);
    CK(hits > 0, "回调被触发", d);

    // ---- 5. 读到的槽位值 ----
    int readOk = 0;
    for (int i = 0; i < pick; ++i) {
        if (!g_slotOk[i]) continue;
        P("  槽 sp+0x%-4x = 0x%lx", g_slotOff[i], g_slotVal[i]);
        readOk++;
    }
    snprintf(d, sizeof d, "%d/%d 个槽位", readOk, pick);
    CK(readOk > 0, "zh_local_get 读取", d);

    // ---- 6. 卸载 ----
    int urc = zh_unhook(fn);
    snprintf(d, sizeof d, "rc=%d", urc);
    CK(urc == 0, "zh_unhook", d);

    P("=== t07 完成 ===");
}
