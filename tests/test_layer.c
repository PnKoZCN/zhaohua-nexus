// 框架门面层测试
//
// 注意：这些测试在 host 上跑，没有真实游戏 .so，
//       所以只验证「未初始化时的行为」和「纯函数逻辑」。
//       真实查找/插桩需要真机。
#include <stdio.h>
#include <string.h>
#include "zh/zh_layer.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else printf("  OK: %s\n", msg); \
} while (0)

int main(void) {
    printf("=== 框架门面 ===\n");

    // 1. 未初始化时的行为：不能崩，要返回可读错误
    CHECK(zh_layer_ready() == 0, "未初始化时 ready == 0");
    CHECK(zh_module_base() == 0, "未初始化时 base == 0");
    CHECK(zh_find("Actor_getHealth") == 0, "未初始化时 zh_find 返回 0");

    unsigned long a = zh_find_verbose("Actor_getHealth");
    CHECK(a == 0, "未初始化时 zh_find_verbose 返回 0");

    // 2. 初始化（会失败，因为 host 上没有 libminecraftpe.so）
    int rc = zh_layer_init_default();
    printf("  zh_layer_init_default() -> %d\n", rc);
    CHECK(rc == ZH_E_NO_BASE || rc == ZH_E_ENGINE || rc == ZH_E_ALREADY,
          "host 上初始化应返回可识别的错误码");

    // 3. 初始化失败后仍然不能崩
    CHECK(zh_layer_ready() == 0 || zh_layer_ready() == 1, "ready 状态可查询");
    zh_find("nonexistent");
    zh_hook("nonexistent", NULL, NULL);

    // 4. 错误码可读化
    CHECK(strlen(ii_strerror(ZH_E_NO_BASE)) > 0, "ZH_E_NO_BASE 有可读消息");
    CHECK(strlen(ii_strerror(ZH_E_ENGINE)) > 0, "ZH_E_ENGINE 有可读消息");

    // 5. 列表查询不崩
    int n = zh_list_functions(NULL, 0);
    printf("  已知函数总数: %d\n", n);
    CHECK(n > 0, "函数表非空");

    const char* names[5];
    int got = zh_list_functions(names, 5);
    CHECK(got == 5, "能取到 5 个函数名");
    for (int i = 0; i < got; ++i)
        if (names[i]) printf("    [%d] %s\n", i, names[i]);

    // 6. 自检在未就绪状态要能报告问题
    char buf[1024];
    int problems = zh_selfcheck(buf, sizeof buf);
    printf("  自检问题数: %d\n", problems);
    CHECK(problems > 0, "未初始化时自检应报告问题");
    printf("--- 自检输出 ---\n%s---\n", buf);

    printf(fails ? "test_layer: %d 项失败\n" : "test_layer: 全部通过\n", fails);
    return fails ? 1 : 0;
}
