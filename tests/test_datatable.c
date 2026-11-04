// 数据表一致性测试：N_ 宏必须等于数组实际条目数
#include <stdio.h>
#include "rettype_db.h"
#include "fields_real.h"
#include "blcall_db.h"
#include "frame_db.h"
#include "const_db.h"
#include "poolhint_db.h"

static int fails = 0;

#define CHECK(arr, macro) do { \
    size_t n = sizeof(arr)/sizeof(arr[0]); \
    if (n != (size_t)(macro)) { \
        printf("FAIL: %s 有 %zu 条，%s = %d\n", #arr, n, #macro, (int)(macro)); \
        fails++; \
    } else { \
        printf("  OK: %-14s %6zu 条\n", #arr, n); \
    } \
} while (0)

int main(void) {
    printf("=== 数据表一致性 ===\n");
    CHECK(g_rettype,  N_RETTYPE);
    CHECK(g_fieldacc, N_FIELDACC);
    CHECK(g_blcall,   N_BLCALL);
    CHECK(g_slot,     N_SLOT);
    CHECK(g_frame,    N_FRAME);
    CHECK(g_const,    N_CONST);
    CHECK(g_poolhint, N_POOLHINT);
    printf(fails ? "test_datatable: %d 项失败\n" : "test_datatable: 全部通过\n", fails);
    return fails ? 1 : 0;
}
