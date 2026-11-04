// 站点表测试：只验证计数/查找，不真的装 hook
#include <stdio.h>
#include "zh/instr_hook.h"

static int fails = 0;
#define CHECK(c,m) do { if(!(c)){printf("FAIL: %s\n",m);fails++;} else printf("  OK: %s\n",m);} while(0)

static void dummy_cb(ii_ctx* c, void* u) { (void)c; (void)u; }

int main(void) {
    printf("=== 站点表（分段数组）===\n");

    CHECK(ii_init() == 0, "ii_init");
    CHECK(ii_count() == 0, "初始 0 站点");

    // 用非法地址（NULL / 0）测错误路径，绝不触发回调
    int rc = ii_install(0, dummy_cb, NULL);
    printf("  ii_install(0) -> %d\n", rc);
    CHECK(rc == -1, "空地址返回 -1");

    // 不可插桩地址（堆上随便一处）
    static int fake[4];
    rc = ii_install((unsigned long)fake, dummy_cb, NULL);
    printf("  ii_install(heap) -> %d\n", rc);
    CHECK(rc == -5 || rc == -1, "堆地址返回 -5（不可插桩）");
    CHECK(rc != -3, "★ 绝不能是 -3（表满）");

    CHECK(ii_count() == 0, "计数仍为 0");
    CHECK(ii_find((unsigned long)fake) == NULL, "查不到");

    printf(fails ? "test_sites: %d 项失败\n" : "test_sites: 全部通过\n", fails);
    return fails ? 1 : 0;
}
