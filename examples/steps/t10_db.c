// 步骤 10：运行期数据表（zh_db_load + zh_tbl_attach）
//
// 验证：把 zh_db_full.bin mmap 进来挂到引擎上，
//       之后所有查询都走这份数据（而不是编译期静态数组）。
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include "zh/zh_layer.h"
#include "zh/zh_db.h"
#include "zh/zh_tbl.h"

static int g_fd = -1;
static void P(const char* fmt, ...) {
    if (g_fd < 0) {
        // ★ 用 O_APPEND：重复注入不覆盖上次结果，便于对比
        g_fd = open("/data/data/com.netease.x19/cache/zh_t10.txt",
                    O_WRONLY|O_CREAT|O_APPEND, 0666);
        if (g_fd < 0) return;
    }
    char b[512]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (n > 0) { b[n++]='\n'; (void)!write(g_fd, b, n); }
}
static void CK(int c, const char* n, const char* d) {
    P("%s %-30s %s", c?"OK  ":"FAIL", n, d?d:"");
}

static zh_db g_db;

__attribute__((constructor))
static void run(void) {
    P("");
    P("════════ 运行 #%d ════════", (int)time(NULL));
    P("=== t10: 运行期数据表 ===");

    const char* dbp = "/data/data/com.netease.x19/cache/zh_db_full.bin";
    char d[160];

    // ---- 0. 挂接前：走编译期静态表 ----
    int att0 = zh_tbl_attached();
    snprintf(d, sizeof d, "%d（0=编译期静态表）", att0);
    CK(att0 == 0, "挂接前 zh_tbl_attached", d);

    int staticFuncs = zh_tbl_count(ZH_TBL_RETFUNC);
    snprintf(d, sizeof d, "%d 个", staticFuncs);
    CK(staticFuncs > 0, "静态表 retfunc 条目", d);

    // ---- 1. 加载数据文件 ----
    int rc = zh_db_load(&g_db, dbp);
    snprintf(d, sizeof d, "rc=%d", rc);
    CK(rc == ZH_DB_OK, "zh_db_load", d);
    if (rc != ZH_DB_OK) { P("  文件: %s", dbp); return; }

    snprintf(d, sizeof d, "%zu 字节, %u 张表", g_db.size, g_db.nTables);
    CK(g_db.size > 0 && g_db.nTables == 10, "文件头", d);

    const char* ver = zh_db_game_version(&g_db);
    CK(ver && *ver, "游戏版本", ver ? ver : "(空)");

    // ---- 2. 挂接到引擎 ----
    int arc = zh_tbl_attach(&g_db);
    snprintf(d, sizeof d, "rc=%d", arc);
    CK(arc == 0, "zh_tbl_attach", d);

    int att1 = zh_tbl_attached();
    snprintf(d, sizeof d, "%d（1=运行期数据表）", att1);
    CK(att1 == 1, "挂接后 zh_tbl_attached", d);

    // ---- 3. 挂接后：查询走 mmap ----
    int nfuncs = zh_tbl_count(ZH_TBL_RETFUNC);
    snprintf(d, sizeof d, "%d 个（静态表是 %d 个）", nfuncs, staticFuncs);
    CK(nfuncs > 0 && nfuncs == staticFuncs, "retfunc 条目数一致", d);

    // ---- 4. 逐表点条目数 ----
    static const struct { int type; const char* name; } TB[] = {
        {ZH_TBL_RETTYPE,  "rettype" }, {ZH_TBL_BLCALL,  "blcall"  },
        {ZH_TBL_CONST,    "const"   }, {ZH_TBL_FRAME,   "frame"   },
        {ZH_TBL_SLOT,     "slot"    }, {ZH_TBL_FIELDACC,"fieldacc"},
        {ZH_TBL_RETPOINT, "retpoint"}, {ZH_TBL_CONSTFUNC,"constfunc"},
        {ZH_TBL_RETFUNC,  "retfunc" }, {ZH_TBL_POOLHINT,"poolhint"},
    };
    int total = 0, ntab = 0;
    for (unsigned k = 0; k < sizeof TB/sizeof TB[0]; ++k) {
        int c = zh_tbl_count(TB[k].type);
        if (c > 0) { ntab++; total += c; }
        P("  %-11s %6d 条", TB[k].name, c);
    }
    snprintf(d, sizeof d, "%d 张表 / %d 条", ntab, total);
    CK(ntab == 10 && total > 28000, "全部 10 张表", d);

    // ---- 5. 读一条真实数据验证内容 ----
    // 5a. 栈帧
    uint32_t fs = 0, ss = 0; int sc = 0;
    if (zh_tbl_frame_by_name("ItemStack_isNull", &fs, &ss, &sc) == 0) {
        snprintf(d, sizeof d, "frame=0x%x slots=%d", fs, sc);
        CK(fs == 0x440 && sc == 18, "frame_by_name(ItemStack_isNull)", d);
    } else {
        CK(0, "frame_by_name(ItemStack_isNull)", "查不到");
    }

    // 5b. 字段
    int found = 0;
    for (int i = 0; i < zh_tbl_count(ZH_TBL_FIELDACC); ++i) {
        const char* f = NULL; uint16_t off = 0; int base = 0, sz = 0, wr = 0;
        if (zh_tbl_fieldacc(i, &f, &off, &base, &sz, &wr) != 0) continue;
        if (f && strcmp(f, "Actor_getLevel") == 0) {
            snprintf(d, sizeof d, "+0x%x base=x%d size=%d %s",
                     off, base, sz, wr ? "写" : "读");
            CK(off == 0x250, "fieldacc(Actor_getLevel)", d);
            found = 1; break;
        }
    }
    if (!found) CK(0, "fieldacc(Actor_getLevel)", "查不到");

    // 5c. 字符串池能取出来
    if (zh_tbl_retfunc(0)) {
        CK(1, "zh_tbl_retfunc(0)", zh_tbl_retfunc(0));
    } else CK(0, "zh_tbl_retfunc(0)", "空");

    // ---- 6. 退回静态表 ----
    zh_tbl_attach(NULL);
    int att2 = zh_tbl_attached();
    snprintf(d, sizeof d, "%d", att2);
    CK(att2 == 0, "zh_tbl_attach(NULL) 退回", d);

    int back = zh_tbl_count(ZH_TBL_RETFUNC);
    snprintf(d, sizeof d, "%d 个", back);
    CK(back == staticFuncs, "退回后条目数仍正确", d);

    // ---- 7. 卸载 ----
    zh_db_unload(&g_db);
    P("=== t10 完成 ===");
}
