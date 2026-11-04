// 错误码映射完整性测试
#include <stdio.h>
#include <string.h>
#include "zh/zlog.h"

static int fails = 0;

static void expect(int code, const char* must_contain) {
    const char* s = ii_strerror(code);
    if (!s || !*s) { printf("FAIL: %d 无消息\n", code); fails++; return; }
    if (must_contain && !strstr(s, must_contain)) {
        printf("FAIL: %d = \"%s\" 不含 \"%s\"\n", code, s, must_contain);
        fails++;
    }
}

int main(void) {
    expect(0, "成功");
    expect(-1, "参数");
    expect(-2, "已安装");
    expect(-3, "满");
    expect(-5, "不可插桩");
    expect(-7, "16");
    expect(-11, "128MB");
    expect(-17, "超时");

    // 未知码不能返回 NULL
    if (!ii_strerror(-999)) { printf("FAIL: 未知码返回 NULL\n"); fails++; }

    // 建议表：已知码要有内容
    const int with_hint[] = {-2,-3,-5,-7,-11,-13,-15,-16,-17,-18};
    for (unsigned i = 0; i < sizeof(with_hint)/sizeof(with_hint[0]); ++i) {
        const char* h = ii_strhint(with_hint[i]);
        if (!h || !*h) { printf("FAIL: %d 无建议\n", with_hint[i]); fails++; }
    }

    printf(fails ? "test_strerror: %d 项失败\n" : "test_strerror: 全部通过\n", fails);
    return fails ? 1 : 0;
}
