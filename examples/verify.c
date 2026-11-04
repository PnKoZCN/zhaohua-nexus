// ===========================================================================
// 真机验证模块：逐项检验引擎能力
//
// 所有结果写到一个文件，由宿主机读回来核对。
// 每一项都是「能验证 / 不能验证」二值，不做猜测。
// ===========================================================================
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include "zh/zh_layer.h"
#include "zh/zh_db.h"

// 表抽象层（内部 API，测试用）
int zh_tbl_attach(zh_db* db);
int zh_tbl_count(int type);
int zh_tbl_attached(void);

static volatile uint64_t g_hits = 0;
static volatile uint64_t g_retHits = 0;

static void P(const char* fmt, ...) {
    static int fd = -1;
    if (fd < 0) {
        fd = open("/data/data/com.netease.x19/cache/zh_verify.txt",
                  O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if (fd < 0) fd = open("/data/local/tmp/zh_verify.txt",
                              O_WRONLY|O_CREAT|O_TRUNC, 0666);
    }
    if (fd < 0) return;
    char b[512];
    va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (n > 0) { if (n > (int)sizeof b - 2) n = sizeof b - 2; b[n++] = '\n'; (void)!write(fd, b, n); }
}

#define OK(cond, name, ...) do { \
    char _info[256]; \
    snprintf(_info, sizeof _info, __VA_ARGS__); \
    P("%s %-24s %s", (cond) ? "OK  " : "FAIL", name, _info); \
} while (0)

static void on_hit(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);
}
static void on_ret(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_add_fetch(&g_retHits, 1, __ATOMIC_RELAXED);
}

static unsigned long find_base(const char* name) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512]; unsigned long b = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, name)) continue;
        unsigned long lo; if (sscanf(line, "%lx-", &lo) == 1) { if (!b || lo < b) b = lo; }
    }
    fclose(f);
    return b;
}

__attribute__((constructor))
static void verify_main(void) {
    P("=== ZhaoHua Engine 真机验证 ===");
    P("self base(libminecraftpe) = 0x%lx", find_base("libminecraftpe.so"));

    // ---- 1. 初始化 ----
    zh_layer_cfg cfg = ZH_LAYER_CFG_DEFAULT;
    cfg.logPath  = "/data/data/com.netease.x19/cache/zh_verify_layer.log";
    cfg.logLevel = ZLOG_DEBUG;
    int rc = zh_layer_init(&cfg);
    OK(rc == ZH_OK || rc == ZH_E_ALREADY, "zh_layer_init", "rc=%d", rc);
    if (!zh_layer_ready()) { P("未就绪，终止"); return; }

    // ---- 2. 状态 ----
    char st[512];
    zh_layer_status(st, sizeof st);
    P("status: %s", st);
    OK(zh_module_base() != 0, "模块基址", "0x%lx", zh_module_base());
    OK(zh_game_version() != 0, "版本指纹", "0x%llx", (unsigned long long)zh_game_version());

    // ---- 3. 数据表 ----
    int nf = zh_list_functions(NULL, 0);
    OK(nf > 0, "函数表", "%d 条", nf);

    // ---- 4. 按名字查找 ----
    const char* testFn = "Actor_getHealth";
    unsigned long a = zh_find_verbose(testFn);
    OK(a != 0, "zh_find", "%s -> 0x%lx", testFn, a);

    // ---- 5. 自检 ----
    char chk[1024];
    int probs = zh_selfcheck(chk, sizeof chk);
    P("--- selfcheck (%d 问题) ---\n%s", probs, chk);
    OK(probs >= 0, "zh_selfcheck", "%d 问题", probs);

    // ---- 6. 插桩 ----
    int h1 = zh_hook(testFn, on_hit, NULL);
    OK(h1 == 0, "zh_hook(HEAD)", "%s rc=%d", testFn, h1);

    // ---- 7. 同一地址挂第二个回调（多回调链）----
    int h2 = ii_add_callback(a, on_hit, (void*)1, 100);
    OK(h2 == 0, "ii_add_callback", "rc=%d", h2);
    int cbc = ii_callback_count(a);
    OK(cbc == 2, "回调链计数", "%d 个", cbc);

    // ---- 8. 返回点插桩 ----
    int h3 = zh_hook_ret(testFn, -1, on_ret, NULL);
    OK(h3 == 0, "zh_hook_ret", "%s rc=%d", testFn, h3);

    // ---- 9. 归属 ----
    ii_set_owner(a, "verify_mod");
    const char* ow = ii_get_owner(a);
    OK(ow && !strcmp(ow, "verify_mod"), "归属设置", "%s", ow ? ow : "(null)");

    // ---- 10. 运行期表 ----

    int nBefore = zh_tbl_count(ZH_TBL_RETTYPE);
    OK(nBefore > 0, "编译期表", "rettype %d", nBefore);

    zh_db db;
    int ldr = zh_db_load(&db, "/data/data/com.netease.x19/files/zh_db_full.bin");
    if (ldr == ZH_DB_OK) {
        zh_tbl_attach(&db);
        int nAfter = zh_tbl_count(ZH_TBL_RETTYPE);
        OK(nAfter == nBefore, "运行期表", "rettype %d (一致)", nAfter);
        P("db ver = %s", zh_db_game_version(&db));
        zh_tbl_attach(NULL);
    } else {
        P("SKIP 运行期表  (数据文件未部署, rc=%d)", ldr);
    }

    // ---- 11. 参数/返回类型查询 ----
    int kind = ii_ret_kind(a);
    const char* tn = ii_ret_type_name(a);
    OK(kind != II_RET_UNKNOWN, "返回类型", "%s (kind=%d)", tn, kind);

    // ---- 12. 上下文转储（不崩即通过）----
    ii_ctx tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.pc = a; tmp.x[0] = 0xb4000079a5b2a000ULL; tmp.x[17] = 0x34155555;
    zh_dump_ctx(&tmp);
    OK(1, "zh_dump_ctx", "已输出到 stderr");

    P("=== 安装完毕，等待命中 ===");
    P("hits=%llu retHits=%llu",
      (unsigned long long)g_hits, (unsigned long long)g_retHits);
}
