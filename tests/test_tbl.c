// 表抽象层测试：验证编译期表与运行时表结果一致
#include <stdio.h>
#include <string.h>
#include "zh/zh_db.h"
extern int zh_tbl_attach(zh_db* db);
extern int zh_tbl_attached(void);
extern int zh_tbl_count(int type);
extern int zh_tbl_rettype(int i, const char** name, uint32_t* off, int* kind, const char** typeName);
extern int zh_tbl_blcall(int i, const char** func, uint32_t* callOff, const char** target, uint32_t* targetOff);
extern const char* zh_tbl_constfunc(int i);
extern int zh_tbl_rettype_by_name(const char* name, int* kind, const char** typeName);

#include "rettype_db.h"
#include "blcall_db.h"
#include "const_db.h"

static int fails = 0;
#define CHECK(c,m) do { if(!(c)){printf("FAIL: %s\n",m);fails++;} else printf("  OK: %s\n",m);} while(0)

int main(int argc, char** argv) {
    printf("=== 表抽象层 ===\n");

    // 1. 未 attach：走编译期表
    CHECK(zh_tbl_attached() == 0, "初始未 attach");
    int n1 = zh_tbl_count(ZH_TBL_RETTYPE);
    printf("  rettype(编译期): %d\n", n1);
    CHECK(n1 == N_RETTYPE, "count 与编译期宏一致");

    // 2. 编译期表内容抽查
    const char *fn; uint32_t off; int kind; const char* tn;
    zh_tbl_rettype(0, &fn, &off, &kind, &tn);
    printf("  [0] %s off=0x%x kind=%d type=%s\n", fn, off, kind, tn ? tn : "");
    CHECK(fn && !strcmp(fn, g_rettype[0].name), "第 0 条名字一致");
    CHECK(off == g_rettype[0].off, "第 0 条偏移一致");

    // 3. 按名字查
    int k2; const char* t2;
    CHECK(zh_tbl_rettype_by_name("Actor_getHealth", &k2, &t2) == 0, "按名字能查到");
    printf("  Actor_getHealth: kind=%d type=%s\n", k2, t2 ? t2 : "");
    CHECK(k2 == 4, "kind=4 (FLOAT)");

    // 4. attach 数据文件后结果应相同
    const char* path = (argc > 1) ? argv[1] : "zh_db.bin";
    zh_db db;
    if (zh_db_load(&db, path) == ZH_DB_OK) {
        zh_tbl_attach(&db);
        CHECK(zh_tbl_attached() == 1, "attach 成功");

        int n2 = zh_tbl_count(ZH_TBL_RETTYPE);
        printf("  rettype(运行期): %d\n", n2);
        CHECK(n2 == n1, "两张表条目数一致");

        // 抽查同一条
        zh_tbl_rettype(0, &fn, &off, &kind, &tn);
        CHECK(fn && !strcmp(fn, g_rettype[0].name), "运行期表第 0 条名字一致");
        CHECK(off == g_rettype[0].off, "运行期表第 0 条偏移一致");

        // blcall 抽查
        const char *bf, *bt; uint32_t boff, btoff;
        zh_tbl_blcall(0, &bf, &boff, &bt, &btoff);
        printf("  blcall[0]: %s +0x%x -> %s\n", bf, boff, bt);
        CHECK(bf && !strcmp(bf, g_blcall[0].func), "blcall[0] 一致");

        // constfunc
        CHECK(!strcmp(zh_tbl_constfunc(0), g_constfunc[0]), "constfunc[0] 一致");

        // 切回编译期
        zh_tbl_attach(NULL);
        CHECK(zh_tbl_attached() == 0, "可以切回编译期");
        CHECK(zh_tbl_count(ZH_TBL_RETTYPE) == n1, "切回后条目数正确");
    } else {
        printf("  （无数据文件，跳过运行期表对比）\n");
    }

    printf(fails ? "test_tbl: %d 项失败\n" : "test_tbl: 全部通过\n", fails);
    return fails ? 1 : 0;
}
