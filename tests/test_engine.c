// 引擎实例化测试：验证多实例隔离
#include <stdio.h>
#include <string.h>
#include "zh/instr_hook.h"

static int fails = 0;
#define CHECK(c,m) do { if(!(c)){printf("FAIL: %s\n",m);fails++;} else printf("  OK: %s\n",m);} while(0)

static int g_aCalls = 0, g_bCalls = 0;
static void cb_a(ii_ctx* c, void* u) { (void)c;(void)u; g_aCalls++; }
static void cb_b(ii_ctx* c, void* u) { (void)c;(void)u; g_bCalls++; }

int main(void) {
    printf("=== 引擎实例化 ===\n");

    // 1. 默认实例存在且唯一
    ii_engine* d1 = ii_default();
    ii_engine* d2 = ii_default();
    CHECK(d1 != NULL, "默认实例存在");
    CHECK(d1 == d2, "默认实例是同一个");

    // 2. 创建两个独立实例
    ii_engine* A = ii_engine_create();
    ii_engine* B = ii_engine_create();
    CHECK(A && B, "创建两个实例");
    CHECK(A != B && A != d1 && B != d1, "实例互不相同");

    // 3. 初始状态隔离
    CHECK(ii_engine_count(A) == 0, "A 初始 0 站点");
    CHECK(ii_engine_count(B) == 0, "B 初始 0 站点");
    CHECK(ii_engine_count(d1) == 0, "默认初始 0 站点");

    // 4. 统计隔离
    ii_engine_stats sa, sb;
    CHECK(ii_engine_get_stats(A, &sa) == 0, "A 统计可读");
    CHECK(ii_engine_get_stats(B, &sb) == 0, "B 统计可读");
    CHECK(sa.nSites == 0 && sb.nSites == 0, "统计初始为 0");
    printf("  A: sites=%d cap=%d traceSeq=%u\n", sa.nSites, sa.capacity, sa.traceSeq);

    // 5. 追踪状态隔离
    ii_engine_trace_begin(A);
    CHECK(ii_engine_get_stats(A, &sa) == 0 && sa.traceOn == 1, "A 开启追踪");
    CHECK(ii_engine_get_stats(B, &sb) == 0 && sb.traceOn == 0, "B 不受影响");
    ii_engine_trace_end(A);
    CHECK(ii_engine_get_stats(A, &sa) == 0 && sa.traceOn == 0, "A 关闭追踪");

    // 6. 查找隔离（都没装站点，应该都查不到）
    CHECK(ii_engine_find(A, 0x1000) == NULL, "A 查不到");
    CHECK(ii_engine_find(B, 0x1000) == NULL, "B 查不到");

    // 7. 销毁不崩
    ii_engine_destroy(A);
    ii_engine_destroy(B);
    CHECK(1, "销毁不崩");

    // 8. 默认实例不能被销毁
    ii_engine_destroy(d1);
    CHECK(ii_default() != NULL, "默认实例销毁被忽略");

    // 9. NULL 安全
    CHECK(ii_engine_count(NULL) == 0, "NULL 实例 count 返回 0");
    CHECK(ii_engine_find(NULL, 0) == NULL, "NULL 实例 find 返回 NULL");
    CHECK(ii_engine_get_stats(NULL, &sa) != 0, "NULL 实例 stats 返回错误");
    ii_engine_destroy(NULL);
    CHECK(1, "NULL 销毁不崩");

    printf(fails ? "test_engine: %d 项失败\n" : "test_engine: 全部通过\n", fails);
    return fails ? 1 : 0;
}
