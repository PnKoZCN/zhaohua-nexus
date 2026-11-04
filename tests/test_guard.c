// 崩溃隔离测试：回调里故意制造 SIGSEGV，验证不会带崩进程
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include "zh/instr_hook.h"

static int fails = 0;
#define CHECK(c,m) do { if(!(c)){printf("FAIL: %s\n",m);fails++;} else printf("  OK: %s\n",m);} while(0)

static int g_ok_ran = 0;

// 正常回调
static void cb_ok(ii_ctx* c, void* u) { (void)c;(void)u; g_ok_ran++; }

// 故意崩溃的回调（读 NULL）
static void cb_crash(ii_ctx* c, void* u) {
    (void)c; (void)u;
    volatile int* p = (volatile int*)0;
    *p = 1;                      // SIGSEGV
}

int main(void) {
    printf("=== 崩溃隔离 ===\n");

    long before = ii_guard_caught();

    // 1. 正常回调应返回 1
    int r1 = ii_guard_call(cb_ok, NULL, NULL);
    CHECK(r1 == 1, "正常回调返回 1");
    CHECK(g_ok_ran == 1, "正常回调被执行");

    // 2. 崩溃回调应返回 0，且不终止进程
    int r2 = ii_guard_call(cb_crash, NULL, NULL);
    CHECK(r2 == 0, "崩溃回调返回 0（已隔离）");

    long after = ii_guard_caught();
    printf("  隔离计数: %ld -> %ld\n", before, after);
    CHECK(after > before, "崩溃计数增加");

    // 3. 崩溃后还能继续正常工作
    int r3 = ii_guard_call(cb_ok, NULL, NULL);
    CHECK(r3 == 1, "崩溃后仍可正常调用");
    CHECK(g_ok_ran == 2, "计数正确");

    // 4. NULL 回调安全
    CHECK(ii_guard_call(NULL, NULL, NULL) == 1, "NULL 回调安全");

    printf(fails ? "test_guard: %d 项失败\n" : "test_guard: 全部通过\n", fails);
    return fails ? 1 : 0;
}
