// ===========================================================================
// 示例 2：进阶 —— 字段读写 + 返回值修改 + 重定向
// ===========================================================================
#include <stdio.h>
#include "zh/zh_layer.h"

// --- 场景 A：读实体血量，低于 5 时打日志 ---
static void on_get_health(ii_ctx* c, void* user) {
    (void)user;
    // Actor_getHealth 返回 float 在 s0
    float hp = zh_get_ret_float(c);
    if (hp < 5.0f)
        LOG_I(LOG_HOOK, "低血量: %.1f", hp);
}

// --- 场景 B：读 this 的某个字段 ---
static void on_actor_tick(ii_ctx* c, void* user) {
    (void)user;
    // Actor 的 Level 指针在 +0x250
    void* level = ZH_THIS_PTR(c, 0x250);
    if (!level) return;
    static int once = 0;
    if (!once) { once = 1; LOG_I(LOG_HOOK, "Level = %p", level); }
}

// --- 场景 C：改返回值 ---
static void on_get_max_stack(ii_ctx* c, void* user) {
    (void)user;
    zh_set_ret_int(c, 255);   // 无论如何都返回 255
}

// --- 场景 D：调用重定向（把某个调用换掉）---
static void* redirect_target(ii_ctx* c, void* user) {
    (void)user;
    // c->pc 就是调用点地址
    unsigned long site = c->pc;
    // 原目标要查表
    unsigned long orig = ii_redirect_orig_target(site);
    (void)orig;
    return (void*)orig;   // 原样转发；可换成别的函数地址
}

__attribute__((constructor))
static void init(void) {
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return;

    zh_hook_ret("Actor_getHealth",    -1, on_get_health, NULL);
    zh_hook    ("Actor_normalTick",       on_actor_tick, NULL);
    zh_hook    ("ItemStack_getMaxStackSize", on_get_max_stack, NULL);

    zh_redirect("Inventory_addItem", "ItemStack_getCount",
                redirect_target, NULL);

    // 打印某个函数的栈槽（@Local 替代）
    zh_dump_locals("ItemStack_isNull");

    LOG_I(LOG_REG, "已安装 %d 个 hook", zh_active_count());
}
