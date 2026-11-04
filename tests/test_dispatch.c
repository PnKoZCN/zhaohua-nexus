// ===========================================================================
// ii_dispatch 集成测试 —— 直接验证「回调链真的被遍历」
//
// ★ 这个测试的存在理由：
//   之前 ii_dispatch 读 s->cb（老字段），而安装写 s->cbs（链表），
//   导致 FULL 模式回调永远不执行。host 测试当时只测了链表插入，
//   没测 dispatch 是否调用它，所以漏了。
// ===========================================================================
#include <stdio.h>
#include <string.h>
#include "zh/instr_hook.h"

static int fails = 0;
#define CHECK(c,m) do { if(!(c)){printf("FAIL: %s\n",m);fails++;} else printf("  OK: %s\n",m);} while(0)

static int g_order[8];
static int g_n = 0;

static void cb_a(ii_ctx* c, void* u) { (void)c;(void)u; if(g_n<8) g_order[g_n++]=1; }
static void cb_b(ii_ctx* c, void* u) { (void)c;(void)u; if(g_n<8) g_order[g_n++]=2; }
static void cb_c(ii_ctx* c, void* u) { (void)c;(void)u; if(g_n<8) g_order[g_n++]=3; }
static void cb_cancel(ii_ctx* c, void* u) { (void)u; if(g_n<8) g_order[g_n++]=9; ii_cancel(c); }

static void reset(void) { g_n = 0; memset(g_order, 0, sizeof g_order); }

int main(void) {
    printf("=== ii_dispatch 集成测试 ===\n");

    if (ii_init() != 0) { printf("FAIL ii_init\n"); return 1; }
    ii_test_clear_sites();

    ii_ctx ctx;
    memset(&ctx, 0, sizeof ctx);

    // ---- 1. 单回调被调用 ----
    reset();
    int sid = ii_test_register_site(0x1000, cb_a, NULL, 100);
    CHECK(sid >= 0, "注册假站点");
    void* r = ii_dispatch(&ctx, (uint64_t)sid);
    CHECK(g_n == 1 && g_order[0] == 1, "★ 单回调被 dispatch 调用");
    // 假站点没有真 trampoline，返回 NULL 是预期的；
    // 真机上 dispatch 会返回 trampoline 地址（见 t05c）。
    CHECK(r == NULL, "假站点返回 NULL（无 trampoline，预期）");

    // ---- 2. 多回调按 priority 降序 ----
    reset();
    ii_test_add_cb(0x1000, cb_b, NULL, 200);   // 高
    ii_test_add_cb(0x1000, cb_c, NULL, 50);    // 低
    ii_dispatch(&ctx, (uint64_t)sid);
    printf("  执行顺序: %d %d %d\n", g_order[0], g_order[1], g_order[2]);
    CHECK(g_n == 3, "★ 三个回调都被调用");
    CHECK(g_order[0]==2 && g_order[1]==1 && g_order[2]==3,
          "★ 按 priority 降序（200→100→50）");

    // ---- 3. dead 节点被跳过 ----
    reset();
    ii_test_kill_cb(0x1000, cb_b);
    ii_dispatch(&ctx, (uint64_t)sid);
    printf("  跳过 dead 后: %d %d\n", g_order[0], g_order[1]);
    CHECK(g_n == 2, "★ dead 回调被跳过");

    // ---- 4. cancel 后停止 ----
    ii_test_clear_sites();
    reset();
    int sid2 = ii_test_register_site(0x2000, cb_cancel, NULL, 300);  // 先执行并 cancel
    ii_test_add_cb(0x2000, cb_c, NULL, 100);                          // 应被跳过
    ii_dispatch(&ctx, (uint64_t)sid2);
    printf("  cancel 后执行数: %d\n", g_n);
    CHECK(g_n == 1, "★ cancel 后不再执行后续回调");

    // ---- 5. 边界安全 ----
    ii_test_clear_sites();
    CHECK(ii_dispatch(&ctx, 999999) == NULL, "越界 slotId 返回 NULL");
    CHECK(ii_dispatch(NULL, 0) == NULL, "NULL ctx 不崩");

    ii_ctx_light lctx;
    memset(&lctx, 0, sizeof lctx);
    CHECK(ii_dispatch_light(&lctx, 999999) == NULL, "LIGHT 越界不崩");

    printf(fails ? "\ntest_dispatch: %d 项失败\n" : "\ntest_dispatch: 全部通过\n", fails);
    return fails ? 1 : 0;
}
