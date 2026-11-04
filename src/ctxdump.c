#include "zh/instr_hook.h"
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>

// 语义提示表（来自真机跨轮验证，20_Local变量推断表.md）
static const char* zh_reg_hint(int i) {
    switch (i) {
        case 0:  return "this / 返回值";
        case 4:  return "-1 哨兵";
        case 5:  return "时间戳(候选)";
        case 11: return "常量 0x3FFFE";
        case 12: return "常量 8";
        case 13: return "常量 0xCE6B34F6";
        case 14: return "x5 的副本";
        case 15: return "高精度计数器";
        case 17: return "常量 0x34155555";
        case 21: return "常量 6524";
        case 27: return "常量 6840";
        case 28: return "状态码(候选)";
        default: return "";
    }
}

// 值类型推断（按 Android arm64 地址布局）
static const char* zh_val_kind(uint64_t v) {
    if (v == 0) return "null";
    if (v == 0xFFFFFFFFFFFFFFFFULL || v == 0x00FFFFFFFFFFFFFFULL) return "-1";
    if (v < 0x10000ULL) return "small";
    if (0xB400000000000000ULL <= v && v < 0xB800000000000000ULL) return "heap";
    if (0x7B0000000000ULL <= v && v < 0x7F0000000000ULL) return "so";
    if (0x7F0000000000ULL <= v && v < 0x800000000000ULL) return "stack";
    if (v < 0x100000000ULL) return "u32";
    return "?";
}

void ii_dump_ctx_range(ii_ctx* c, int lo, int hi) {
    if (!c) return;
    if (lo < 0) lo = 0;
    if (hi > 30) hi = 30;

    char buf[4096];
    int n = 0;
    n += snprintf(buf+n, sizeof(buf)-n,
        "=== ctx  pc=0x%llx  sp=0x%llx  pstate=0x%llx ===\n",
        (unsigned long long)c->pc, (unsigned long long)c->sp,
        (unsigned long long)c->pstate);

    for (int i = lo; i <= hi && n < (int)sizeof(buf)-96; ++i) {
        const char* h = zh_reg_hint(i);
        const char* k = zh_val_kind(c->x[i]);
        if (h[0])
            n += snprintf(buf+n, sizeof(buf)-n, "  x%-2d 0x%016llx  [%-5s] %s\n",
                          i, (unsigned long long)c->x[i], k, h);
        else
            n += snprintf(buf+n, sizeof(buf)-n, "  x%-2d 0x%016llx  [%-5s]\n",
                          i, (unsigned long long)c->x[i], k);
    }
    ssize_t w = write(2, buf, (size_t)n);
    (void)w;
}

void ii_dump_ctx(ii_ctx* c) { ii_dump_ctx_range(c, 0, 30); }
