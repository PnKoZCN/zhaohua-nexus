// 共享区布局与跨探针容量自检
//
// 这些数值决定「同时能注入几个库」。改任何上限都要重新核对本测试，
// 并同步 +1 ZHREG_LAYOUT（否则老探针会按错偏移读共享区）。

#include <stdio.h>
#include <string.h>
#include "zh/zhreg.h"
#include "zh/instr_hook.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  OK   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); g_fail++; } \
} while (0)

int main(void)
{
    printf("=== 共享区布局 ===\n");
    printf("  sizeof(zh_region)      = %zu\n", sizeof(zh_region));
    printf("  ZHREG_FILE_SIZE        = %d\n", ZHREG_FILE_SIZE);
    printf("  sizeof(zh_site_rec)    = %zu\n", sizeof(zh_site_rec));
    printf("  sizeof(zh_pool_rec)    = %zu\n", sizeof(zh_pool_rec));

    CHECK(sizeof(zh_region) <= ZHREG_FILE_SIZE,
          "共享区文件容得下 zh_region");

    printf("\n=== 跨探针容量 ===\n");
    printf("  ZHREG_MAX_CB           = %d  (单站点可挂的探针回调数)\n", ZHREG_MAX_CB);
    printf("  ZHREG_MAX_SITE         = %d  (全进程共享站点数)\n", ZHREG_MAX_SITE);
    printf("  ZHREG_MAX_POOL         = %d\n", ZHREG_MAX_POOL);
    printf("  ZHREG_SITES_PER_POOL   = %d\n", ZHREG_SITES_PER_POOL);

    CHECK(ZHREG_MAX_CB >= 8,  "单站点至少支持 8 个库共存");
    CHECK(ZHREG_MAX_SITE >= 4096, "共享站点表 >= 4096");
    CHECK(ZHREG_MAX_SITE >= II_ENGINE_MAX_OWN,
          "共享站点表不小于单库上限");
    CHECK(ZHREG_SITES_PER_POOL * ZHREG_MAX_POOL >= ZHREG_MAX_SITE,
          "池记录容量覆盖共享站点表");

    printf("\n=== 上限自洽 ===\n");
    printf("  II_MAX_SITES           = %d\n", II_MAX_SITES);
    printf("  II_ENGINE_MAX_OWN      = %d\n", II_ENGINE_MAX_OWN);
    CHECK(II_ENGINE_MAX_OWN <= II_MAX_SITES, "单库上限不超过本地站点表");

    printf("\n=== 布局版本 ===\n");
    printf("  ZHREG_LAYOUT           = %d\n", ZHREG_LAYOUT);
    CHECK(ZHREG_LAYOUT >= 2, "布局版本已递增（结构体变更后必须 +1）");

    printf("\n%s\n", g_fail ? "有失败项" : "全部通过");
    return g_fail ? 1 : 0;
}
