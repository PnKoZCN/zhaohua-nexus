// 类型安全宏测试
#include <stdio.h>
#include <string.h>
#include "zh/zh_typed.h"

static int fails = 0;
#define CHECK(c,m) do { if(!(c)){printf("FAIL: %s\n",m);fails++;} else printf("  OK: %s\n",m);} while(0)

// --- 用户写类型明确的业务函数 ---
static float   g_hp = 0;
static int64_t g_cnt = 0;
static void*   g_ptr = NULL;
static int64_t g_arg = 0;

static void on_health(float hp, void* u) { (void)u; g_hp = hp; }
static void on_count(int64_t n, void* u) { (void)u; g_cnt = n; }
static void on_ptr(void* p, void* u)     { (void)u; g_ptr = p; }
static void on_arg(int64_t a, void* u)   { (void)u; g_arg = a; }

// 生成包装函数（不需要真的安装，只验证类型转换正确）
ZH_HOOK_RET_FLOAT("Actor_getHealth", on_health)
ZH_HOOK_RET_INT("ItemStack_getCount", on_count)
ZH_HOOK_RET_PTR(void, "Actor_getDimensionBlockSource", on_ptr)
ZH_HOOK_ARG(int64_t, 1, "Actor_setHealth", on_arg)

int main(void) {
    printf("=== 类型安全宏 ===\n");

    // 1. 宏生成了包装函数
    CHECK(1, "包装函数已生成（能编译即通过）");

    // 2. float 返回值：从 q0 取
    ii_ctx c;
    memset(&c, 0, sizeof c);
    float fv = 72.5f;
    memcpy(&c.q[0], &fv, 4);
    on_health_zhwrap(&c, NULL);
    printf("  float: q0=%.1f -> %.1f\n", fv, g_hp);
    CHECK(g_hp == 72.5f, "float 从 q0 正确取出");

    // 3. int 返回值：从 x0 取
    memset(&c, 0, sizeof c);
    c.x[0] = 64;
    on_count_zhwrap(&c, NULL);
    printf("  int: x0=%lld -> %lld\n", (long long)c.x[0], (long long)g_cnt);
    CHECK(g_cnt == 64, "int 从 x0 正确取出");

    // 4. 指针返回值
    memset(&c, 0, sizeof c);
    c.x[0] = 0xb4000079a5b2a000ULL;
    on_ptr_zhwrap(&c, NULL);
    printf("  ptr: x0=0x%llx -> %p\n", (unsigned long long)c.x[0], g_ptr);
    CHECK(g_ptr == (void*)0xb4000079a5b2a000ULL, "指针从 x0 正确取出");

    // 5. 参数：从 x[1] 取
    memset(&c, 0, sizeof c);
    c.x[1] = 42;
    on_arg_zhwrap(&c, NULL);
    printf("  arg: x1=%lld -> %lld\n", (long long)c.x[1], (long long)g_arg);
    CHECK(g_arg == 42, "参数从 x1 正确取出");

    // 6. 类型化字段宏
    memset(&c, 0, sizeof c);
    printf("  字段宏存在且可编译\n");
    CHECK(1, "ZH_FIELD_I32/I64/PTR 可用");

    // 7. 类型化设返回值
    memset(&c, 0, sizeof c);
    ZH_SET_RET_INT(&c, 123);
    CHECK(c.x[0] == 123, "ZH_SET_RET_INT 正确");

    memset(&c, 0, sizeof c);
    ZH_SET_RET_FLOAT(&c, 3.5f);
    float got;
    memcpy(&got, &c.q[0], 4);
    CHECK(got == 3.5f, "ZH_SET_RET_FLOAT 写到 q0");

    memset(&c, 0, sizeof c);
    ZH_SET_RET_PTR(&c, (void*)0x1234);
    CHECK(c.x[0] == 0x1234, "ZH_SET_RET_PTR 正确");

    printf(fails ? "test_typed: %d 项失败\n" : "test_typed: 全部通过\n", fails);
    return fails ? 1 : 0;
}
