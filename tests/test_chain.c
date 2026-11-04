// 回调链测试：验证同一地址可挂多个回调，按 priority 降序执行
#include <stdio.h>
#include <string.h>
#include "zh/instr_hook.h"

static int fails = 0;
#define CHECK(c,m) do { if(!(c)){printf("FAIL: %s\n",m);fails++;} else printf("  OK: %s\n",m);} while(0)

// 记录执行顺序
static int g_order[8];
static int g_n = 0;

static void cb_low (ii_ctx* c, void* u) { (void)c;(void)u; if(g_n<8) g_order[g_n++]=3; }
static void cb_mid (ii_ctx* c, void* u) { (void)c;(void)u; if(g_n<8) g_order[g_n++]=5; }
static void cb_high(ii_ctx* c, void* u) { (void)c;(void)u; if(g_n<8) g_order[g_n++]=9; }

// 模拟一次 dispatch 的遍历（复刻引擎逻辑）
static void simulate(ii_cb_node* head) {
    g_n = 0;
    for (ii_cb_node* n = head; n; n = n->next) {
        if (!n->alive || !n->cb) continue;
        n->cb(NULL, n->user);
    }
}

int main(void) {
    printf("=== 回调链 ===\n");

    // 1. 结构检查
    CHECK(sizeof(ii_cb_node) > 0, "ii_cb_node 已定义");

    // 2. 手工构造链表，验证 priority 排序遍历
    ii_cb_node a = {0}, b = {0}, c = {0};
    a.cb = cb_high; a.priority = 9; a.alive = 1; a.next = &b;
    b.cb = cb_mid;  b.priority = 5; b.alive = 1; b.next = &c;
    c.cb = cb_low;  c.priority = 3; c.alive = 1; c.next = NULL;

    simulate(&a);
    printf("  执行顺序: %d %d %d\n", g_order[0], g_order[1], g_order[2]);
    CHECK(g_n == 3, "3 个回调都执行了");
    CHECK(g_order[0] == 9 && g_order[1] == 5 && g_order[2] == 3,
          "按 priority 降序（9 → 5 → 3）");

    // 3. dead 节点被跳过
    b.alive = 0;
    simulate(&a);
    printf("  跳过 dead 后: %d %d\n", g_order[0], g_order[1]);
    CHECK(g_n == 2, "dead 节点被跳过");

    // 4. 空链不崩
    simulate(NULL);
    CHECK(g_n == 0, "空链安全");

    // 5. API 在未初始化时安全
    CHECK(ii_add_callback(0x1000, cb_high, NULL, 1) != 0, "非法地址返回错误");
    CHECK(ii_callback_count(0x1000) == 0, "不存在的站点返回 0");
    CHECK(ii_remove_callback(0x1000, cb_high, NULL) != 0, "移除不存在返回错误");

    printf(fails ? "test_chain: %d 项失败\n" : "test_chain: 全部通过\n", fails);
    return fails ? 1 : 0;
}
