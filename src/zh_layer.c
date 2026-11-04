// ===========================================================================
// ZhaoHua Nexus —— 框架接入层实现
// ===========================================================================
#include "zh/zh_layer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

// 数据表
#include "zinfo_db.h"
#include "frame_db.h"
#include "blcall_db.h"
#include "poolhint_db.h"

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
static int          g_ready    = 0;
static unsigned long g_base    = 0;
static uint64_t     g_version  = 0;
static char         g_logPath[256] = "/data/data/com.netease.x19/cache/zh_layer.log";
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

// 最近一次失败的结构化信息
static zh_err_info g_lastErr = {0, NULL, 0, 0, 0, NULL};

static void set_err(int code, const char* func, unsigned long addr, const char* detail) {
    g_lastErr.code         = code;
    g_lastErr.funcName     = func;
    g_lastErr.addr         = addr;
    g_lastErr.siteCount    = ii_count();
    g_lastErr.siteCapacity = II_MAX_SITES;
    g_lastErr.detail       = detail;
}

const zh_err_info* zh_last_err(void) { return &g_lastErr; }

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

// 找模块基址
static unsigned long find_module_base(const char* name) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    unsigned long lo = 0, best = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, name)) continue;
        if (sscanf(line, "%lx-", &lo) != 1) continue;
        if (!best || lo < best) best = lo;
    }
    fclose(f);
    return best;
}

// 文件头 64K 的哈希（用于版本指纹）
static uint64_t fingerprint(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char buf[65536];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (!n) return 0;
    // FNV-1a 64
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= buf[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// 从 /proc/self/maps 找模块实际路径
static int module_path(char* out, int len) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[512];
    int ok = -1;
    while (fgets(line, sizeof line, f)) {
        char* p = strstr(line, "libminecraftpe.so");
        if (!p) continue;
        // 路径在最后一列
        char* s = strchr(line, '/');
        if (!s) continue;
        char* e = s;
        while (*e && *e != '\n') ++e;
        int n = (int)(e - s);
        if (n <= 0 || n >= len) continue;
        memcpy(out, s, (size_t)n);
        out[n] = 0;
        ok = 0;
        break;
    }
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
int zh_layer_init(const zh_layer_cfg* cfg) {
    pthread_mutex_lock(&g_mx);
    if (g_ready) { pthread_mutex_unlock(&g_mx); return ZH_E_ALREADY; }

    zh_layer_cfg def = ZH_LAYER_CFG_DEFAULT;
    if (!cfg) cfg = &def;

    if (cfg->logPath) {
        strncpy(g_logPath, cfg->logPath, sizeof(g_logPath) - 1);
        g_logPath[sizeof(g_logPath) - 1] = 0;
    }
    zlog_init(g_logPath);
    zlog_set_level(cfg->logLevel >= 0 ? cfg->logLevel : ZLOG_INFO);
    zlog_set_mode(ZLOG_MODE_FILE);

    // 1. 引擎初始化
    int rc = ii_init();
    if (rc != 0) {
        LOG_E(LOG_REG, "ii_init 失败 rc=%d (%s)", rc, ii_strerror(rc));
        pthread_mutex_unlock(&g_mx);
        return ZH_E_ENGINE;
    }
    LOG_I(LOG_REG, "引擎已初始化");

    // 2. 找模块基址
    g_base = find_module_base("libminecraftpe.so");
    if (!g_base) {
        LOG_E(LOG_REG, "找不到 libminecraftpe.so");
        LOG_I(LOG_REG, "本进程可能不是游戏进程");
        pthread_mutex_unlock(&g_mx);
        return ZH_E_NO_BASE;
    }
    LOG_I(LOG_REG, "模块基址 = 0x%lx", g_base);

    // 3. 版本指纹
    char mpath[512];
    if (module_path(mpath, sizeof mpath) == 0) {
        g_version = fingerprint(mpath);
        LOG_I(LOG_REG, "版本指纹 = 0x%llx  (%s)",
              (unsigned long long)g_version, mpath);
    }

    // 4. 预注册 trampoline 池
    if (cfg->registerPools) {
        int n = ii_register_hint_pools(g_poolhint, N_POOLHINT);
        LOG_I(LOG_POOL, "注册 %d 个池提示（共 %d 条）", n, N_POOLHINT);
    }

    // 5. CFI（可选）
    if (cfg->cfiPath) {
        int c = ii_frame_cfi_load(cfg->cfiPath);
        if (c == 0)
            LOG_I(LOG_LOCAL, "CFI 已加载，%d 个函数", ii_frame_cfi_loaded());
        else
            LOG_W(LOG_LOCAL, "CFI 加载失败 rc=%d（栈帧精度受限）", c);
    } else {
        LOG_I(LOG_LOCAL, "未指定 CFI 路径，使用静态扫描的栈帧数据");
    }

    // 6. 数据表自检
    if (N_FUNCINFO == 0 || N_FRAME == 0) {
        LOG_E(LOG_REG, "数据表为空（N_FUNCINFO=%d N_FRAME=%d）",
              N_FUNCINFO, N_FRAME);
        pthread_mutex_unlock(&g_mx);
        return ZH_E_NO_DB;
    }
    LOG_I(LOG_REG, "数据表: %d 函数 / %d 栈槽 / %d 调用点",
          N_FUNCINFO, N_SLOT, N_BLCALL);

    g_ready = 1;
    pthread_mutex_unlock(&g_mx);

    LOG_I(LOG_REG, "=== ZhaoHua Nexus 就绪 ===");
    LOG_I(LOG_REG, "  基址 0x%lx  指纹 0x%llx",
          g_base, (unsigned long long)g_version);
    LOG_I(LOG_REG, "  已知函数 %d 个，调用点 %d 个，栈槽 %d 个",
          N_FUNCINFO, N_BLCALL, N_SLOT);
    return ZH_OK;
}

int zh_layer_init_default(void) {
    return zh_layer_init(NULL);
}

int zh_layer_ready(void) { return g_ready; }
unsigned long zh_module_base(void) { return g_base; }
uint64_t zh_game_version(void) { return g_version; }

int zh_layer_status(char* buf, int len) {
    if (!buf || len <= 0) return 0;
    int n = snprintf(buf, (size_t)len,
        "ready=%d base=0x%lx ver=0x%llx hooks=%d/%d "
        "funcs=%d calls=%d slots=%d",
        g_ready, g_base, (unsigned long long)g_version,
        ii_count(), II_MAX_SITES,     // ★ 之前两边都传 ii_count()，永远显示 N/N
        N_FUNCINFO, N_BLCALL, N_SLOT);
    return n;
}

const char* zh_layer_summary(void) {
    static char s[256];
    zh_layer_status(s, sizeof s);
    return s;
}

// ---------------------------------------------------------------------------
// 函数查找
// ---------------------------------------------------------------------------

unsigned long zh_find(const char* funcName) {
    if (!funcName || !g_ready) return 0;
    for (int i = 0; i < N_FUNCINFO; ++i) {
        if (g_funcinfo[i].name && strcmp(g_funcinfo[i].name, funcName) == 0)
            return g_base + g_funcinfo[i].off;
    }
    return 0;
}

unsigned long zh_find_verbose(const char* funcName) {
    if (!funcName) {
        LOG_E(LOG_HOOK, "zh_find: funcName 为 NULL");
        return 0;
    }
    if (!g_ready) {
        LOG_E(LOG_HOOK, "zh_find: 未初始化，先调 zh_layer_init()");
        return 0;
    }
    unsigned long a = zh_find(funcName);
    if (a) {
        LOG_I(LOG_HOOK, "找到 \"%s\" @ 0x%lx (base+0x%lx)",
              funcName, a, a - g_base);
        return a;
    }

    // 找不到：给出建议
    LOG_E(LOG_HOOK, "函数 \"%s\" 不在数据表中", funcName);
    LOG_I(LOG_HOOK, "  数据表只有 %d 个函数，游戏总函数约 10 万+", N_FUNCINFO);
    LOG_I(LOG_HOOK, "  可能原因：");
    LOG_I(LOG_HOOK, "    1. 名字拼写不同（数据表用 Class_method 形式）");
    LOG_I(LOG_HOOK, "    2. 该函数未收录，需用逆向工具自行找偏移");
    LOG_I(LOG_HOOK, "    3. 游戏版本不同（数据表对应 3.10.5.301327）");

    // 模糊匹配：找包含关系的前 5 个
    int shown = 0;
    for (int i = 0; i < N_FUNCINFO && shown < 5; ++i) {
        if (g_funcinfo[i].name && strstr(g_funcinfo[i].name, funcName)) {
            LOG_I(LOG_HOOK, "    相似: %s (base+0x%x)",
                  g_funcinfo[i].name, g_funcinfo[i].off);
            ++shown;
        }
    }
    if (!shown && strlen(funcName) > 3) {
        // 反向：用数据表的名字找用户输入
        char head[20];
        snprintf(head, sizeof head, "%.*s", 6, funcName);
        for (int i = 0; i < N_FUNCINFO && shown < 5; ++i) {
            if (g_funcinfo[i].name && strstr(head, g_funcinfo[i].name)) {
                LOG_I(LOG_HOOK, "    相似: %s", g_funcinfo[i].name);
                ++shown;
            }
            if (strlen(head) < 3) break;
        }
    }
    return 0;
}

int zh_exists(const char* funcName) {
    return zh_find(funcName) != 0;
}

int zh_list_functions(const char** out, int max) {
    if (!out) return N_FUNCINFO;
    int n = 0;
    for (int i = 0; i < N_FUNCINFO && n < max; ++i)
        if (g_funcinfo[i].name) out[n++] = g_funcinfo[i].name;
    return n;
}

// ---------------------------------------------------------------------------
// 插桩
// ---------------------------------------------------------------------------

static int hook_common(const char* funcName, int light, ii_callback cb,
                       ii_light_callback lcb, void* user, int prio, int which) {
    unsigned long a = zh_find_verbose(funcName);
    if (!a) { set_err(ZH_E_ARG, funcName, 0, "函数不在数据表"); return ZH_E_ARG; }

    // ★ 先看是不是"已经装过了"。
    //   装过的话该地址当前是 b/ldr 跳转指令，ii_can_instrument() 必然
    //   判否 —— 若先做那个检查，多实例/多次挂同一个函数就会被误拒
    //   （t11 里全局引擎挂 A 已装的地址，就是在这里拿到 -5）。
    //   已有站点时直接追加回调链，这才是对标 Mixin 多 @Inject 的语义。
    int already = (which == 0 && ii_find(a) != NULL);

    if (!already && !ii_can_instrument(a)) {
        LOG_E(LOG_HOOK, "\"%s\" 处不可插桩", funcName);
        LOG_I(LOG_HOOK, "  %s", ii_strhint(-5));
        set_err(-5, funcName, a, "指令不可插桩（分支/PC相对/使用x16x17）");
        return -5;
    }

    int rc;
    switch (which) {
        case 0: // 入口
            rc = light ? ii_install_light_ex(a, lcb, user, prio)
                       : ii_install_ex(a, cb, user, prio);
            break;
        case 1: { // 返回点
            unsigned long ra = 0;
            int r = ii_retpoint_addr(funcName, -1, &ra);
            if (r != 0 || !ra) {
                LOG_E(LOG_HOOK, "\"%s\" 无可用返回点 rc=%d", funcName, r);
                LOG_I(LOG_HOOK, "  该函数没有 ret 指令，或不在返回点表中");
                return r ? r : -5;
            }
            rc = ii_install(ra - 4, cb, user);
            break;
        }
        case 2: { // 尾部
            rc = ii_install_at_tail(funcName, cb, user);
            break;
        }
        default: return ZH_E_ARG;
    }

    if (rc == 0)
        LOG_I(LOG_HOOK, "✓ %s @0x%lx (%s)", funcName, a,
              which == 0 ? "入口" : which == 1 ? "返回点" : "尾部");
    else {
        LOG_E(LOG_HOOK, "✗ %s rc=%d (%s)", funcName, rc, ii_strerror(rc));
        const char* h = ii_strhint(rc);
        if (h && *h) LOG_I(LOG_HOOK, "  %s", h);
        set_err(rc, funcName, a, h);
    }
    return rc;
}

int zh_hook(const char* funcName, ii_callback cb, void* user) {
    return hook_common(funcName, 0, cb, NULL, user, II_PRIO_DEFAULT, 0);
}
int zh_hook_prio(const char* funcName, ii_callback cb, void* user, int prio) {
    return hook_common(funcName, 0, cb, NULL, user, prio, 0);
}
int zh_hook_light(const char* funcName, ii_light_callback cb, void* user) {
    return hook_common(funcName, 1, NULL, cb, user, II_PRIO_DEFAULT, 0);
}
int zh_hook_ret(const char* funcName, int nth, ii_callback cb, void* user) {
    (void)nth;
    return hook_common(funcName, 0, cb, NULL, user, II_PRIO_DEFAULT, 1);
}
int zh_hook_tail(const char* funcName, ii_callback cb, void* user) {
    return hook_common(funcName, 0, cb, NULL, user, II_PRIO_DEFAULT, 2);
}

int zh_hook_call(const char* funcName, uint32_t callOff, ii_callback cb, void* user) {
    unsigned long a = zh_find_verbose(funcName);
    if (!a) return ZH_E_ARG;
    int rc = ii_install(a + callOff, cb, user);
    if (rc == 0)
        LOG_I(LOG_HOOK, "✓ %s+0x%x 调用点", funcName, callOff);
    else
        LOG_E(LOG_HOOK, "✗ %s+0x%x rc=%d (%s)", funcName, callOff,
              rc, ii_strerror(rc));
    return rc;
}

// 带崩溃隔离的插桩
static void guarded_trampoline(ii_ctx* c, void* user) {
    // user 里存的是 {cb, user} 对
    struct pair { ii_callback cb; void* u; };
    struct pair* p = (struct pair*)user;
    ii_guard_call(p->cb, c, p->u);
}

int zh_hook_guarded(const char* funcName, ii_callback cb, void* user) {
    struct pair { ii_callback cb; void* u; };
    struct pair* p = (struct pair*)malloc(sizeof *p);
    if (!p) return -9;
    p->cb = cb;
    p->u  = user;
    int rc = zh_hook(funcName, guarded_trampoline, p);
    if (rc != 0) { free(p); return rc; }
    LOG_I(LOG_HOOK, "%s 已启用崩溃隔离", funcName);
    return 0;
}

long zh_guard_caught(void) { return ii_guard_caught(); }

// ---------------------------------------------------------------------------
// 常量修改
// ---------------------------------------------------------------------------
int zh_modify_const(const char* funcName, uint32_t value, int nth,
                    ii_callback cb, void* user) {
    if (!funcName) { LOG_E(LOG_HOOK, "zh_modify_const: 函数名为 NULL"); return ZH_E_ARG; }
    if (!zh_find(funcName)) { LOG_E(LOG_HOOK, "函数 \"%s\" 不在数据表", funcName); return ZH_E_ARG; }

    int rc = ii_install_at_const(funcName, value, nth, cb, user);
    if (rc == 0)
        LOG_I(LOG_HOOK, "✓ %s 的常量 0x%x 已插桩", funcName, value);
    else {
        LOG_E(LOG_HOOK, "✗ %s 常量 0x%x rc=%d (%s)",
              funcName, value, rc, ii_strerror(rc));
        LOG_I(LOG_HOOK, "  用 zh_const_count(\"%s\", 0x%x) 查是否存在", funcName, value);
    }
    return rc;
}

int zh_const_count(const char* funcName, uint32_t value) {
    if (!funcName) return 0;
    if (!zh_find(funcName)) {
        LOG_E(LOG_HOOK, "函数 \"%s\" 不在数据表", funcName);
        return 0;
    }
    return ii_const_count(funcName, value);
}

// ---------------------------------------------------------------------------
// 重定向
// ---------------------------------------------------------------------------
int zh_redirect(const char* hostFunc, const char* targetFunc,
                zh_redirect_fn cb, void* user) {
    if (!hostFunc || !targetFunc) {
        LOG_E(LOG_HOOK, "zh_redirect: 参数为 NULL");
        return ZH_E_ARG;
    }
    if (!zh_find(hostFunc))   { LOG_E(LOG_HOOK, "宿主函数 \"%s\" 不在数据表", hostFunc); return ZH_E_ARG; }
    if (!zh_find(targetFunc)) { LOG_E(LOG_HOOK, "目标函数 \"%s\" 不在数据表", targetFunc); return ZH_E_ARG; }

    // 引擎已经实现了「按名字查 blcall 表 + 安装」，直接用
    int rc = ii_redirect_name(hostFunc, targetFunc, -1,
                              cb, user);   // 签名已与 ii_redirect_cb 一致
    if (rc == 0)
        LOG_I(LOG_HOOK, "✓ 重定向 %s → %s", hostFunc, targetFunc);
    else {
        LOG_E(LOG_HOOK, "✗ 重定向 %s → %s rc=%d (%s)",
              hostFunc, targetFunc, rc, ii_strerror(rc));
        LOG_I(LOG_HOOK, "  数据表只有 %d 个调用点，可能未覆盖此组合", N_BLCALL);
    }
    return rc;
}

// ---------------------------------------------------------------------------
// 字段访问
// ---------------------------------------------------------------------------
int64_t zh_field_get(ii_ctx* c, uint8_t argIdx, uint16_t off, uint8_t size) {
    return ii_field_read(c, argIdx, off, size);
}
void zh_field_set(ii_ctx* c, uint8_t argIdx, uint16_t off, uint8_t size, int64_t v) {
    ii_field_write(c, argIdx, off, size, v);
}

// ---------------------------------------------------------------------------
// 局部变量
// ---------------------------------------------------------------------------
int64_t zh_local_get(ii_ctx* c, uint16_t spOff, uint8_t size) {
    return ii_local_read(c, spOff, size);
}
void zh_local_set(ii_ctx* c, uint16_t spOff, uint8_t size, int64_t v) {
    ii_local_write(c, spOff, size, v);
}

int zh_locals_of(const char* funcName, int* outOffsets, int* outSizes, int max) {
    if (!funcName) return 0;
    for (int i = 0; i < N_FRAME; ++i) {
        if (!g_frame[i].func || strcmp(g_frame[i].func, funcName) != 0) continue;
        int n = 0;
        for (int k = 0; k < g_frame[i].slotCount && n < max; ++k) {
            const zh_slot* s = &g_slot[g_frame[i].slotStart + k];
            if (outOffsets) outOffsets[n] = s->spOff;
            if (outSizes)   outSizes[n]   = s->size;
            ++n;
        }
        return n;
    }
    return 0;
}

void zh_dump_locals(const char* funcName) {
    for (int i = 0; i < N_FRAME; ++i) {
        if (!g_frame[i].func || strcmp(g_frame[i].func, funcName) != 0) continue;
        LOG_I(LOG_LOCAL, "=== %s  栈帧 0x%x  %d 个槽位 ===",
              funcName, g_frame[i].frameSize, g_frame[i].slotCount);
        for (int k = 0; k < g_frame[i].slotCount; ++k) {
            const zh_slot* s = &g_slot[g_frame[i].slotStart + k];
            LOG_I(LOG_LOCAL, "  sp+0x%-4x  %d 字节  (首次写入 @+0x%x)",
                  s->spOff, s->size, s->insnOff);
        }
        return;
    }
    LOG_E(LOG_LOCAL, "函数 \"%s\" 不在栈帧表中", funcName);
    LOG_I(LOG_LOCAL, "  栈帧表只有 %d 个函数（CFI 可提供更多）", N_FRAME);
}

// ---------------------------------------------------------------------------
// 返回值
// ---------------------------------------------------------------------------
int zh_set_ret_int(ii_ctx* c, int64_t v)   { if (!c) return -1; ii_set_ret_int(c, v); return 0; }
int zh_set_ret_float(ii_ctx* c, float v)   { if (!c) return -1; ii_set_ret_float(c, v); return 0; }
int zh_set_ret_ptr(ii_ctx* c, void* v)     { if (!c) return -1; ii_set_ret_ptr(c, v); return 0; }
void*   zh_get_ret_ptr(ii_ctx* c)         { return c ? (void*)(uintptr_t)c->x[0] : NULL; }
int64_t zh_get_ret_int(ii_ctx* c)         { return c ? (int64_t)c->x[0] : 0; }
float   zh_get_ret_float(ii_ctx* c) {
    // ★ float 在 q0 的低 32 位，必须按位重解释。
    //   (float)c->q[0] 是整数转换，实测 72.5 会得到 1116798976.0。
    if (!c) return 0.0f;
    union { __uint128_t q; float f; } u;
    u.q = c->q[0];
    return u.f;
}

void zh_cancel(ii_ctx* c) { if (c) ii_cancel(c); }

int zh_make_cancelable(const char* funcName) {
    unsigned long a = zh_find_verbose(funcName);
    if (!a) return ZH_E_ARG;
    int rc = ii_set_cancelable(a, 1);
    if (rc == 0) LOG_I(LOG_HOOK, "✓ %s 已允许 cancel", funcName);
    else LOG_E(LOG_HOOK, "✗ %s cancelable rc=%d", funcName, rc);
    return rc;
}

// ---------------------------------------------------------------------------
// 运行期管理
// ---------------------------------------------------------------------------
int zh_set_owner(const char* funcName, const char* owner) {
    unsigned long a = zh_find(funcName);
    if (!a) { set_err(ZH_E_ARG, funcName, 0, "函数不在数据表"); return ZH_E_ARG; }
    ii_set_owner(a, owner);
    LOG_I(LOG_HOOK, "%s 归属设为 \"%s\"", funcName, owner ? owner : "(null)");
    return 0;
}

int zh_unhook_owner(const char* owner) {
    if (!owner) return ZH_E_ARG;
    int n = ii_uninstall_owner(owner);
    LOG_I(LOG_HOOK, "卸载归属 \"%s\" 的 %d 个 hook", owner, n);
    return n;
}

int zh_unhook(const char* funcName) {
    if (!funcName) return ZH_E_ARG;
    unsigned long a = zh_find(funcName);
    if (!a) { LOG_E(LOG_HOOK, "unhook: 找不到 %s", funcName); return ZH_E_ARG; }

    // ★ 一个函数可能有多个站点：入口、返回点（ret-4）、尾部。
    //   逐个尝试卸载，全部尝试完再判断结果。
    int uninstalled = 0;
    int lastRc = 0;

    // 1. 入口
    if (ii_find(a)) {
        lastRc = ii_uninstall(a);
        if (lastRc == 0) uninstalled++;
    }

    // 2. 所有返回点（ret-4）
    int nret = ii_retpoint_count(funcName);
    for (int i = 0; i < nret; ++i) {
        unsigned long ra = 0;
        if (ii_retpoint_addr(funcName, i, &ra) != 0 || !ra) continue;
        if (!ii_find(ra - 4)) continue;
        lastRc = ii_uninstall(ra - 4);
        if (lastRc == 0) uninstalled++;
    }

    // 3. 尾部（最后一个返回点，与上面重叠，已覆盖）

    if (uninstalled == 0) {
        LOG_E(LOG_HOOK, "✗ unhook %s：找不到任何站点 (rc=%d)", funcName, lastRc);
        return lastRc ? lastRc : -1;
    }
    LOG_I(LOG_HOOK, "✓ unhook %s：卸载 %d 个站点", funcName, uninstalled);
    return 0;
}
int zh_unhook_all(void) { ii_uninstall_all(); LOG_I(LOG_HOOK, "全部卸载"); return 0; }

int zh_enable(const char* funcName, int on) {
    unsigned long a = zh_find(funcName);
    if (!a) { LOG_E(LOG_HOOK, "enable: 找不到 %s", funcName); return ZH_E_ARG; }
    return ii_set_enabled(a, on);
}
int zh_apply(void) { return ii_apply_pending(); }
int zh_active_count(void) { return ii_count(); }

// ---------------------------------------------------------------------------
// 诊断
// ---------------------------------------------------------------------------
// 引擎里的 ii_dump_ctx 未在 instr_hook.h 声明，这里前向声明
void ii_dump_ctx(ii_ctx* c);
void zh_dump_ctx(ii_ctx* c) { if (c) ii_dump_ctx(c); }
void zh_trace_on(void)      { ii_trace_begin(); }
void zh_trace_off(void)     { ii_trace_end(); }
void zh_trace_dump(void)    { ii_trace_dump(); }
int  zh_install_crash_handler(void) { return ii_trace_install_crash_handler(); }

int zh_selfcheck(char* buf, int len) {
    int problems = 0;
    char line[256];
    int n = 0;

    #define ADD(...) do { \
        int k = snprintf(line, sizeof line, __VA_ARGS__); \
        if (k > 0 && n + k < len) { memcpy(buf + n, line, (size_t)k); n += k; } \
    } while (0)

    if (!g_ready)            { ADD("[X] 未初始化\n"); problems++; }
    else                     { ADD("[OK] 已初始化\n"); }

    if (!g_base)             { ADD("[X] 模块基址为 0\n"); problems++; }
    else                     { ADD("[OK] 基址 0x%lx\n", g_base); }

    if (N_FUNCINFO == 0)     { ADD("[X] 函数表为空\n"); problems++; }
    else                     { ADD("[OK] 函数表 %d 条\n", N_FUNCINFO); }

    if (N_FRAME == 0)        { ADD("[X] 栈帧表为空\n"); problems++; }
    else                     { ADD("[OK] 栈帧表 %d 条，槽位 %d\n", N_FRAME, N_SLOT); }

    if (N_BLCALL == 0)       { ADD("[X] 调用点表为空\n"); problems++; }
    else                     { ADD("[OK] 调用点 %d 条\n", N_BLCALL); }

    int cfi = ii_frame_cfi_loaded();
    if (cfi > 0)             { ADD("[OK] CFI %d 个函数\n", cfi); }
    else                     { ADD("[!] CFI 未加载（栈帧精度受限）\n"); }

    int act = ii_count();
    ADD("[--] 当前 hook %d 个\n", act);

    if (n < len) buf[n] = 0;
    #undef ADD
    return problems;
}
