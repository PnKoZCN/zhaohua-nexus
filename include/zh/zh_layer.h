// ===========================================================================
// ZhaoHua Nexus —— 框架接入层
//
// 引擎（instr_hook.h）提供的是「原子能力」：
//   ii_install / ii_local_read / ii_field_write ...
//
// 本层把它们组合成「开发者直接用的门面」：
//   zh_layer_init()        一次初始化（找 base、加载 CFI、注册池）
//   zh_find(name)          按函数名找地址（而不是记偏移）
//   zh_hook(name, ...)     按函数名插桩
//   zh_hook_ret(name, ...) 按函数名 hook 返回点
//   zh_redirect(name, ...) 按函数名重定向调用
//
// 设计原则：
//   1. 失败必有可读原因（不返回裸错误码）
//   2. 所有偏移从数据表来（不硬编码）
//   3. 一个进程可以安全地多次初始化（幂等）
// ===========================================================================
#pragma once

#include <stdint.h>
#include "zh/instr_hook.h"
#include "zh/zlog.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 结构化错误（失败时保留完整上下文）
// ---------------------------------------------------------------------------
typedef struct {
    int           code;         // 错误码
    const char*   funcName;     // 出错的函数名（可能为 NULL）
    unsigned long addr;         // 相关地址
    int           siteCount;    // 当时站点数
    int           siteCapacity; // 容量
    const char*   detail;       // 额外说明（静态字符串）
} zh_err_info;

// 最近一次失败的结构化信息（每次失败时更新）
const zh_err_info* zh_last_err(void);

// 初始化返回值
#define ZH_OK              0
#define ZH_E_ALREADY       1    // 已初始化（不是错误）
#define ZH_E_NO_BASE      -1    // 找不到 libminecraftpe.so
#define ZH_E_NO_DB        -2    // 数据表加载失败
#define ZH_E_ARG          -3    // 参数无效
#define ZH_E_CFI          -4    // CFI 加载失败
#define ZH_E_ENGINE       -5    // 引擎 ii_init 失败

// 初始化配置
typedef struct {
    const char* logPath;       // NULL = /data/data/com.netease.x19/cache/zh_layer.log
    const char* cfiPath;       // NULL = 不加载 CFI（功能受限）
    int         logLevel;      // ZLOG_INFO
    int         registerPools; // 1 = 用 poolhint_db 预注册池
} zh_layer_cfg;

#define ZH_LAYER_CFG_DEFAULT { NULL, NULL, ZLOG_INFO, 1 }

// 初始化（幂等）
int  zh_layer_init(const zh_layer_cfg* cfg);
int  zh_layer_init_default(void);
int  zh_layer_ready(void);          // 1 = 已就绪
unsigned long zh_module_base(void);// libminecraftpe.so 基址
uint64_t zh_game_version(void);    // 从指纹算的版本标识（0 = 未知）

// 详细状态（写进 buf，返回写入长度）
int zh_layer_status(char* buf, int len);
// 一行摘要
const char* zh_layer_summary(void);

// ---------------------------------------------------------------------------
// 函数查找（按名字，不记偏移）
// ---------------------------------------------------------------------------

// 按函数名找虚拟地址。找不到返回 0。
unsigned long zh_find(const char* funcName);

// 查找并打印诊断（找不到时说清原因）
unsigned long zh_find_verbose(const char* funcName);

// 函数是否存在
int zh_exists(const char* funcName);

// 列出所有已知函数名（写进 buf，返回个数；buf 为 NULL 时只返回总数）
int zh_list_functions(const char** out, int max);

// ---------------------------------------------------------------------------
// 插桩（按名字）
// ---------------------------------------------------------------------------

// 在函数入口插桩
int zh_hook(const char* funcName, ii_callback cb, void* user);

// 带优先级
int zh_hook_prio(const char* funcName, ii_callback cb, void* user, int prio);

// LIGHT 模式（高频函数）
int zh_hook_light(const char* funcName, ii_light_callback cb, void* user);

// 在返回点插桩（第 nth 个 ret，-1 = 全部）
int zh_hook_ret(const char* funcName, int nth, ii_callback cb, void* user);

// 在函数尾部
int zh_hook_tail(const char* funcName, ii_callback cb, void* user);

// 在函数内某次调用点插桩
int zh_hook_call(const char* funcName, uint32_t callOff, ii_callback cb, void* user);

// 带崩溃隔离的插桩：回调崩溃时不会带崩游戏（setjmp 开销约 20-50ns）
int zh_hook_guarded(const char* funcName, ii_callback cb, void* user);

// 已隔离的崩溃次数
long zh_guard_caught(void);

// 常量修改（对标 Mixin @ModifyConstant）
//
// 在函数内「编码了 value 的那条指令」处插桩，回调里改对应寄存器。
// value 必须与数据表里记录的完全一致（26,447 条）。
int zh_modify_const(const char* funcName, uint32_t value, int nth,
                    ii_callback cb, void* user);

// 查某函数内等于 value 的常量出现次数
int zh_const_count(const char* funcName, uint32_t value);

// ---------------------------------------------------------------------------
// 调用重定向（对标 Mixin @Redirect）
// ---------------------------------------------------------------------------

// 重定向回调 —— 签名必须与引擎的 ii_redirect_cb 完全一致
//
// 引擎用 2 个参数调用，多一个都会读到垃圾值。所以这里不做「扩展签名」。
//
// 想拿「原目标地址」：回调里用 ii_redirect_orig_target(c->pc) 查。
// 想拿「调用点地址」：就是 c->pc。
typedef void* (*zh_redirect_fn)(ii_ctx* c, void* user);

// 重定向宿主函数内对 targetName 的调用
int zh_redirect(const char* hostFunc, const char* targetFunc,
                zh_redirect_fn cb, void* user);

// ---------------------------------------------------------------------------
// 字段访问（对标 Mixin @Accessor）
// ---------------------------------------------------------------------------

// 读参数对象字段（argIdx: 0 = this）
int64_t zh_field_get(ii_ctx* c, uint8_t argIdx, uint16_t off, uint8_t size);
void    zh_field_set(ii_ctx* c, uint8_t argIdx, uint16_t off, uint8_t size, int64_t v);

// 便捷宏：读 this 的某个 int32 字段
#define ZH_THIS_I32(c, off)  ((int32_t)zh_field_get((c), 0, (off), 4))
#define ZH_THIS_I64(c, off)  zh_field_get((c), 0, (off), 8)
#define ZH_THIS_PTR(c, off)  ((void*)(uintptr_t)zh_field_get((c), 0, (off), 8))

// ---------------------------------------------------------------------------
// 局部变量（对标 Mixin @Local 的部分替代）
// ---------------------------------------------------------------------------

// 读栈槽（spOff 相对 sp，size 1/2/4/8）
int64_t zh_local_get(ii_ctx* c, uint16_t spOff, uint8_t size);
void    zh_local_set(ii_ctx* c, uint16_t spOff, uint8_t size, int64_t v);

// 按函数名枚举该函数的栈槽（写进 out，返回个数）
// 注意：只知「某偏移存了 N 字节」，不知变量名和类型
int zh_locals_of(const char* funcName, int* outOffsets, int* outSizes, int max);

// 打印某函数的全部栈槽
void zh_dump_locals(const char* funcName);

// ---------------------------------------------------------------------------
// 返回值
// ---------------------------------------------------------------------------

int zh_set_ret_int(ii_ctx* c, int64_t v);
int zh_set_ret_float(ii_ctx* c, float v);
int zh_set_ret_ptr(ii_ctx* c, void* v);
void* zh_get_ret_ptr(ii_ctx* c);
int64_t zh_get_ret_int(ii_ctx* c);
float zh_get_ret_float(ii_ctx* c);

// 取消（对标 Mixin cancel()）—— 必须在 cancelable 站点上调用
void zh_cancel(ii_ctx* c);
int  zh_make_cancelable(const char* funcName);

// ---------------------------------------------------------------------------
// 运行期管理
// ---------------------------------------------------------------------------

// 设置 hook 的归属（模块名），用于分组卸载
int zh_set_owner(const char* funcName, const char* owner);
// 卸载某个归属的全部 hook，返回数量
int zh_unhook_owner(const char* owner);

int zh_unhook(const char* funcName);
int zh_unhook_all(void);
int zh_enable(const char* funcName, int on);
int zh_apply(void);                 // 应用待处理的启停
int zh_active_count(void);          // 当前生效的 hook 数

// ---------------------------------------------------------------------------
// 诊断
// ---------------------------------------------------------------------------

void zh_dump_ctx(ii_ctx* c);                 // 打印 31 寄存器 + 类型
void zh_trace_on(void);
void zh_trace_off(void);
void zh_trace_dump(void);
int  zh_install_crash_handler(void);

// 自检：检查引擎 + 数据表 + 基址，返回问题数（0 = 健康）
int zh_selfcheck(char* buf, int len);

#ifdef __cplusplus
}
#endif
