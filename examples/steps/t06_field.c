// 步骤 6：字段读写（@Accessor 等价）
//
// 目标：Actor_getLevel —— 在 Actor 对象 +0x250 处读 Level 指针。
// 用返回点 hook（此时 x0 = 函数返回值 = Level*），同时验证：
//   1. 能从 this（x[0] 的入参对象）读字段
//   2. ii_field_count / ii_field_at 能查到字段表
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        g_fd = open("/data/data/com.netease.x19/cache/zh_t06.txt",
                    O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (g_fd < 0) return;
    }
    char b[384]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++]='\n'; (void)!write(g_fd, b, n); }
}
static void CK(int c, const char* n, const char* d) {
    P("%s %-30s %s", c?"OK  ":"FAIL", n, d?d:"");
}

static volatile unsigned long g_hits = 0;
static volatile unsigned long g_lastRet = 0;   // 返回的 Level 指针
static volatile unsigned long g_lastField = 0; // 从 this+0x250 读到的值
static volatile int g_match = -1;

static void on_getlevel(ii_ctx* c, void* user) {
    (void)user;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);
    // 返回点：x0 = 函数的返回值（Level*）
    unsigned long ret = (unsigned long)c->x[0];
    g_lastRet = ret;
    // 同时用字段读 API 从 this（argIdx=0，此时 x0 已被返回值覆盖，
    // 所以改用 ii_field_read 需要入口 hook 才准；这里只验证返回值路径）
}

// 入口 hook：x0 = this，可以读 +0x250
static void on_entry(ii_ctx* c, void* user) {
    (void)user;
    // this 在 x[0]
    int64_t v = ii_field_read(c, 0, 0x250, 8);
    g_lastField = (unsigned long)v;
    if (__atomic_load_n(&g_lastRet, __ATOMIC_RELAXED) != 0) {
        g_match = (g_lastField == __atomic_load_n(&g_lastRet, __ATOMIC_RELAXED)) ? 1 : 0;
    }
}

__attribute__((constructor))
static void run(void) {
    P("=== t06: 字段读写 ===");

    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) { P("FAIL init"); return; }
    unsigned long base = zh_module_base();
    P("base = 0x%lx", base);

    const char* fn = "Actor_getLevel";
    unsigned long entry = zh_find(fn);
    if (!entry) { P("FAIL 找不到 %s", fn); return; }
    P("目标 %s @ 0x%lx (base+0x%lx)", fn, entry, entry-base);

    // ---- 1. 字段表查询 ----
    int fc = ii_field_count(fn);
    char d[96]; snprintf(d, sizeof d, "%d 个", fc);
    CK(fc > 0, "ii_field_count", d);

    uint16_t fo; uint8_t fbase, fsize, fwr;
    int rc = ii_field_at(fn, 0, &fo, &fbase, &fsize, &fwr);
    snprintf(d, sizeof d, "+0x%x base=x%d size=%d %s", fo, fbase, fsize, fwr?"写":"读");
    CK(rc == 0, "ii_field_at(0)", d);

    // ---- 2. 入口 hook：读 this+0x250 ----
    int rcA = zh_hook(fn, on_entry, NULL);
    snprintf(d, sizeof d, "rc=%d", rcA);
    CK(rcA == 0, "入口 hook", d);

    // ---- 3. 返回点 hook：读返回值 ----
    int nret = ii_retpoint_count(fn);
    int inst = 0;
    for (int i = 0; i < nret && i < 8; ++i) {
        unsigned long ra = 0;
        if (ii_retpoint_addr(fn, i, &ra) != 0 || !ra) continue;
        if (ii_install(ra - 4, on_getlevel, NULL) == 0) inst++;
    }
    snprintf(d, sizeof d, "%d/%d", inst, nret);
    CK(inst > 0, "返回点 hook", d);

    // ---- 4. 观察 ----
    P("=== 观察 3 秒 ===");
    for (int i = 1; i <= 3; ++i) {
        sleep(1);
        P("  t=%ds  hits=%lu  ret=0x%lx  field=0x%lx  match=%d",
          i, __atomic_load_n(&g_hits, __ATOMIC_RELAXED),
          __atomic_load_n(&g_lastRet, __ATOMIC_RELAXED),
          __atomic_load_n(&g_lastField, __ATOMIC_RELAXED),
          g_match);
    }

    unsigned long hits = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    snprintf(d, sizeof d, "%lu 次", hits);
    CK(hits > 0, "回调被触发", d);

    if (hits > 0) {
        unsigned long ret = __atomic_load_n(&g_lastRet, __ATOMIC_RELAXED);
        unsigned long fld = __atomic_load_n(&g_lastField, __ATOMIC_RELAXED);
        // 合法指针应在 Android 映射区内
        // ★ 正确的 Android 地址范围（之前写成 0xb80000000000 是 48 位，永远失败）
        int retOk = (ret > 0x0000007b00000000UL && ret < 0xb800000000000000UL);
        snprintf(d, sizeof d, "0x%lx", ret);
        CK(retOk, "返回的是合法指针", d);
        snprintf(d, sizeof d, "0x%lx", fld);
        CK(fld > 0x0000007b00000000UL && fld < 0xb800000000000000UL, "读到的字段是合法指针", d);
        snprintf(d, sizeof d, "field==ret: %d", g_match);
        CK(g_match == 1, "字段值与返回值一致", d);
    }

    // ---- 5. 卸载 ----
    int urc = zh_unhook(fn);
    snprintf(d, sizeof d, "rc=%d", urc);
    CK(urc == 0, "zh_unhook", d);

    P("=== t06 完成 ===");
}
