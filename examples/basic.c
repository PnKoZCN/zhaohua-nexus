// ===========================================================================
// 示例 1：最小可用 —— 用框架门面做一次 hook
//
// 对比直接调引擎：
//   引擎：先自己找 base，自己算 base+偏移，自己处理错误码
//   门面：zh_layer_init() + zh_find("函数名") + zh_hook()
// ===========================================================================
#include <stdio.h>
#include <stdint.h>
#include "zh/zh_layer.h"

static volatile uint64_t g_calls = 0;

static void on_tick(ii_ctx* c, void* user) {
    (void)user;
    uint64_t n = __atomic_add_fetch(&g_calls, 1, __ATOMIC_RELAXED);
    if (n % 1000 == 0)
        LOG_I(LOG_HOOK, "normalTick 已调用 %llu 次", (unsigned long long)n);
}

__attribute__((constructor))
static void init(void) {
    // 1. 一次初始化：找 base、注册池、加载数据表
    int rc = zh_layer_init_default();
    if (rc != ZH_OK && rc != ZH_E_ALREADY) {
        LOG_E(LOG_REG, "初始化失败 rc=%d", rc);
        return;
    }

    // 2. 自检
    char buf[1024];
    int problems = zh_selfcheck(buf, sizeof buf);
    LOG_I(LOG_REG, "自检: %d 个问题\n%s", problems, buf);

    // 3. 按名字 hook（不需要记偏移）
    rc = zh_hook("LocalPlayer_normalTick", on_tick, NULL);
    if (rc != 0) {
        LOG_E(LOG_HOOK, "hook 失败 rc=%d (%s)", rc, ii_strerror(rc));
        return;
    }

    LOG_I(LOG_HOOK, "就绪，当前 %d 个 hook", zh_active_count());
}
