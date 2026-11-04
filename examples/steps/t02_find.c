// 步骤 2：只做函数查找，不装任何 hook
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        g_fd = open("/data/data/com.netease.x19/cache/zh_t02.txt",
                    O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (g_fd < 0) return;
    }
    char b[512]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++]='\n'; (void)!write(g_fd, b, n); }
}

static void CK(int cond, const char* name, const char* detail) {
    P("%s %-30s %s", cond ? "OK  " : "FAIL", name, detail ? detail : "");
}

__attribute__((constructor))
static void run(void) {
    P("=== t02: 只做查找 ===");

    int rc = zh_layer_init_default();
    if (rc != ZH_OK && rc != ZH_E_ALREADY) { P("FAIL init rc=%d", rc); return; }
    P("init ok, base=0x%lx", zh_module_base());

    unsigned long base = zh_module_base();

    // ---- 1. 按名字查找已知函数 ----
    const char* known[] = {
        "Actor_getHealth",
        "Actor_getPos",
        "ItemStack_getCount",
        "Block_isAir",
        "LocalPlayer_normalTick",
    };
    for (unsigned i = 0; i < sizeof(known)/sizeof(known[0]); ++i) {
        unsigned long a = zh_find(known[i]);
        char d[128];
        snprintf(d, sizeof d, "0x%lx (base+0x%lx)", a, a ? a - base : 0);
        CK(a != 0, known[i], d);
        if (!a) P("     ↳ 该函数不在 124 条数据表内");
    }

    // ---- 2. zh_exists ----
    char d2[64];
    snprintf(d2, sizeof d2, "exists=%d", zh_exists("Actor_getHealth"));
    CK(zh_exists("Actor_getHealth") == 1, "zh_exists(真实函数)", d2);
    snprintf(d2, sizeof d2, "exists=%d", zh_exists("NoSuchFunction_xyz"));
    CK(zh_exists("NoSuchFunction_xyz") == 0, "zh_exists(假函数)", d2);

    // ---- 3. 枚举全部 ----
    int nf = zh_list_functions(NULL, 0);
    char d3[64]; snprintf(d3, sizeof d3, "%d 个", nf);
    CK(nf > 0, "zh_list_functions(总数)", d3);

    const char* names[8];
    int got = zh_list_functions(names, 8);
    P("  前 %d 个函数:", got);
    for (int i = 0; i < got; ++i) P("    [%d] %s", i, names[i] ? names[i] : "(null)");
    CK(got == 8, "zh_list_functions(取 8 个)", "");

    // ---- 4. 所有 124 个函数都能找到且地址合法 ----
    int valid = 0, invalid = 0;
    const char* all[256];
    int na = zh_list_functions(all, 256);
    for (int i = 0; i < na; ++i) {
        if (!all[i]) { invalid++; continue; }
        unsigned long a = zh_find(all[i]);
        // 合法地址应该在 [base, base+0x14000000) 内
        if (a >= base && a < base + 0x14000000UL) valid++;
        else invalid++;
    }
    char d4[96]; snprintf(d4, sizeof d4, "valid=%d invalid=%d", valid, invalid);
    CK(invalid == 0, "全部函数地址合法", d4);

    // ---- 5. 查找不存在的函数（不应崩溃）----
    unsigned long bad = zh_find("ThisFunctionDoesNotExist_12345");
    char d5[64]; snprintf(d5, sizeof d5, "0x%lx", bad);
    CK(bad == 0, "查不存在的函数返回 0", d5);

    P("=== t02 完成（未装任何 hook）===");
}
