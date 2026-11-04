// ============================================================================
// 指令级插桩（对应 Mixin 的 @At / @ModifyArg / @Inject(RETURN)）
//
// 与函数级 hook 的区别：
//   函数级：只在函数入口接管，参数已是传入值
//   指令级：可以插在任意一条指令上，能拿到「参数已求值、尚未传下去」的时刻
//
// 机制：
//   原指令地址写 4 字节 b -> 薄壳
//   薄壳：mov x17,#站点号 ; b common_handler
//   common_handler：
//     保存 x0-x30 / sp / pc / pstate / q0-q31 / fpsr / fpcr 到栈
//     构造 CpuContext（布局与 zh::backend::CpuContext 一致）
//     调用户回调（回调可读写任何寄存器）
//     从栈恢复
//     跳到该站点的 trampoline
//   站点 trampoline：
//     原指令（必要时重定位）
//     跳回 原地址+4
// ============================================================================
#pragma once

// ===========================================================================
// 架构判定
//
// 指令级插桩只有 ARM64 实现（shell / trampoline / handler 全是 ARM64 汇编）。
// 其它架构上这些 handler 退化为空桩，引擎的数据表 / 编码器 / 链表等
// 与架构无关的部分仍可编译和单测 —— 主机侧 CI 靠这个跑回归。
// ===========================================================================
#if defined(__aarch64__) || defined(_M_ARM64)
#  define ZH_ARCH_ARM64 1
#else
#  define ZH_ARCH_ARM64 0
#endif
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 与 zh::backend::CpuContext 完全一致的布局（800 字节）
typedef struct {
    uint64_t   x[31];      // x0-x30，偏移 0
    uint64_t   sp;         // 偏移 248
    uint64_t   pc;         // 偏移 256（被插桩指令的地址）
    uint64_t   pstate;     // 偏移 264（nzcv）
    __uint128_t q[32];     // 偏移 272，q0-q31
    uint64_t   fpsr;       // 偏移 784
    uint64_t   fpcr;       // 偏移 792
} ii_ctx;                  // 共 800 字节

typedef void (*ii_callback)(ii_ctx* c, void* user);

// ---------------------------------------------------------------------------
// 轻量上下文 —— 高频函数专用
//
// FULL 模式每次命中 ~113 条指令 / 1100 字节内存流量。
// LIGHT 只保存 x0-x7 和 s0-s7，约 25 条指令 / 100 字节，省 78%。
//
// 限制：
//   - 只能读写 x0-x7 / s0-s7
//   - 不能 cancel
//   - 不能改 sp / pc / 其他寄存器
//
// 用于 tick、渲染、碰撞这类每帧几十上百次的热点。
// ---------------------------------------------------------------------------
typedef struct {
    uint64_t    x[8];   // 偏移 0x00：x0-x7
    __uint128_t q[2];   // 偏移 0x40：q0-q1，即 s0-s7
} ii_ctx_light;

typedef void (*ii_light_callback)(ii_ctx_light* c, void* user);

// 安装为轻量模式（高频函数用）
// 返回码同 ii_install；另有：
//  -16  该地址已是另一种 handler 类型的站点（FULL/LIGHT 不能混挂）
//  -15  池不够，无法在 ±128MB 内分配 trampoline（LIGHT）
//  -18  池不够，无法在 ±128MB 内分配 trampoline（FULL/RAW）
//       注：旧的 wideMode（16 字节入口 + suspend）已禁用，原因见 instr_hook.c
int ii_install_light(unsigned long addr, ii_light_callback cb, void* user);

// ---------------------------------------------------------------------------
// 直通模式（函数级 hook，对应 HookEngine::installAt）
//
// detour 直接接管函数入口，不经 handler、不构造 ctx。detour 需要自己通过
// *origOut 调用原函数。
//
// 与「指令级」模式的区别：
//   指令级：shell 压栈保存 x16/x17 + 置站点号，handler 保存全部寄存器再调回调
//   直通  ：shell 只做 16 字节绝对跳转，不动 sp、不碰栈
//
// 因此 trampoline 不需要任何前导（没有 sp 需要恢复），
// 原指令重定位仍复用同一套（b / b.cond / cbz / tbz / ret 全覆盖）。
//
// 代价：x16 在 detour 入口已被跳板破坏。x16/x17 在 AAPCS64 里是 IP0/IP1，
// 函数入口处本来就没有值保证，编译器不会依赖它们，因此安全。
// ---------------------------------------------------------------------------
// ===== 返回码总表（整个引擎统一）=====
//    0   成功
//   -1   参数非法（空指针 / 越界索引）
//   -2   本探针已在此地址装过 / 查不到该站点
//   -3   站点数已满（II_MAX_SITES）/ 表里查不到该名字
//   -4   引擎初始化失败 / 查不到该条目的第 n 个
//   -5   该指令不支持重定位 / 目标地址不可用
//   -6   mmap 失败 / CFI 表版本不符
//   -7   16 字节入口模式要求原 4 条指令可无害覆盖，不满足
//   -9   calloc 失败
//  -10   目标地址所在页无法改成可写（mprotect 被拒）
//  -11   4 字节 b 编码失败（跳转距离超出 ±128MB）
//  -12   trampoline 构建失败
//  -13   next_n_clean 检查失败（后续指令不适合被覆盖）
//  -14   别的探针已占用该地址（替换语义无法叠加）
//  -15   池不够，无法在 ±128MB 内分配 trampoline（LIGHT）
//  -16   该地址已是另一种 handler 类型的站点（FULL/LIGHT/RAW 不能混挂）
//  -17   wideMode 下 3 次重试仍未能暂停全部线程，已放弃（不改代码）
//  -18   （保留）旧 wideMode 禁用码，现已恢复可用
int ii_install_raw(unsigned long addr, void* detour, void** origOut);

// 便利：从 light ctx 读/写浮点参数（s0-s7 分别在 q0/q1 的四个 32 位槽）
static inline float ii_light_get_f(ii_ctx_light* c, int idx) {
    if (idx < 0 || idx >= 8) return 0.0f;
    uint32_t bits = (uint32_t)((c->q[idx >> 2] >> ((idx & 3) * 32)) & 0xFFFFFFFF);
    float f; __builtin_memcpy(&f, &bits, 4);
    return f;
}
static inline void ii_light_set_f(ii_ctx_light* c, int idx, float v) {
    if (idx < 0 || idx >= 8) return;
    uint32_t bits; __builtin_memcpy(&bits, &v, 4);
    int q = idx >> 2, sh = (idx & 3) * 32;
    __uint128_t mask = ((__uint128_t)0xFFFFFFFFULL) << sh;
    c->q[q] = (c->q[q] & ~mask) | ((__uint128_t)bits << sh);
}

// ---------------------------------------------------------------------------
// 回调链节点
//
// 同一个地址可以挂多个回调（对标 Mixin 的多个 @Inject）。
// 按 priority 降序执行：数字大的先跑。
//
// 用侵入式单链表，O(n) 插入但 n 通常 <= 3。
// 单回调时 head 只指向一个节点，热路径没有额外分支。
// ---------------------------------------------------------------------------
typedef struct ii_cb_node {
    struct ii_cb_node* next;
    ii_callback        cb;
    void*              user;
    int                priority;
    int                alive;      // 0 = 已释放（懒删除，遍历时跳过）
    int                owner;      // 归属实例 id（0 = 全局引擎）
    int                slowWarned; // 已经告警过"回调太慢"（只报一次）
} ii_cb_node;

typedef struct {
    unsigned long addr;        // 被插桩指令地址
    unsigned int  orig;        // 原始指令
    unsigned long trampoline;  // 站点 trampoline
    unsigned long shell;       // 薄壳地址（b 的目标）
    void*         user;
    ii_callback   cb;
    int           installed;
    int           wideMode;      // 1 = 16 字节入口（覆盖 4 条），0 = 4 字节
    unsigned long trampSaved;    // wideMode 时保存的原 4 条指令
    int           branchKind;  // 0=普通 1=b/bl 2=b.cond 3=cbz 4=tbz 5=ret
    int           poolIdx;     // 所属 trampoline 池（-1 = 独立 mmap）
    int           enabled;     // 1 = 应该处于插桩状态（目标状态）
    int           pending;     // 1 = 有待应用的启用/禁用变化
    int           cancelable;  // 1 = 允许该站点 cancel（必须是函数入口）
    void*         userCtx;     // 保留
    ii_light_callback lightCb; // 非空则走轻量 handler
    const char*   owner;       // 归属标识（模块名，可为 NULL）
    ii_cb_node*   cbs;         // 回调链（按 priority 降序）
    int           nCbs;        // 链上节点数（含已 dead 的，用于快速判断）
    int           regIdx;      // 在共享 registry 里的站点索引（-1 = 未登记）
    int           retKind;     // 返回类型 ZR_*（安装时从 rettype_db 查得，-1 = 未知）
    int           priority;    // 回调链优先级，越大越先执行
    // ---- 调用重定向（@Redirect 等价）----
    unsigned long callSite;    // 被改写的 bl 指令地址（0 = 非重定向站点）
    unsigned long origTarget;  // 原 bl 目标
    void*         redirCb;     // ii_redirect_cb
    void*         rawDetour;   // 非空 = 直通模式：入口直接跳过去，不经 handler
    int           cbSlowWarned;   // 老路径回调慢告警
} ii_site;

// 站点表采用「分段数组」：固定的块指针表 + 各块独立分配。
//
// 为什么不用 realloc：ii_dispatch 在热路径上按 slotId 直接索引，
// 数组一旦搬家，正在执行的回调就会读到错位的指针。
// 分段数组永不搬移已分配的块，只新增块指针。
//
// 容量上限 = II_SITE_CHUNKS * II_SITE_CHUNK，可按需调大，不需要改逻辑。
// 每个 ii_engine 实例最多自己装多少个站点（用于分组卸载）
// 回调耗时告警阈值（毫秒）。
// 回调运行在被 hook 的游戏线程上，一旦阻塞就冻结游戏；
// 系统 ANR 阈值是 5 秒（Input dispatching timed out）。
// 实测：constructor 里 sleep(2)×15 必崩，sleep(0.5) 勉强能过。
// 这个阈值只用于告警，不阻止执行。
#define II_CB_SLOW_MS      50

#define II_ENGINE_MAX_OWN  128

#define II_SITE_CHUNK   256
#define II_SITE_CHUNKS  1024
#define II_MAX_SITES    (II_SITE_CHUNK * II_SITE_CHUNKS)   // 262,144

// 引擎实例（类型前置声明，完整定义见文件末尾）
typedef struct ii_engine ii_engine;
ii_engine* ii_default(void);




// 初始化（分配公共 handler 与薄壳池）
int ii_init(void);

// 用离线分析出的池位置直接注册（跳过运行时扫描）
// 扫描 275MB 需要 60 秒，游戏会 ANR；离线表只要毫秒级。
// hints: {vaddr, size} 数组，base 由内部从 dl_iterate_phdr 取
int ii_register_hint_pools(const void* hints, int n);

// 在 addr 这条指令上装插桩
// 返回 0 = 成功
int ii_install(unsigned long addr, ii_callback cb, void* user);

// 在当前站点上再挂一个回调（同一地址可挂多个，按 priority 降序执行）
// 返回 0 = 成功；-2 = 站点不存在（用 ii_install 先建）
int ii_add_callback(unsigned long addr, ii_callback cb, void* user, int priority);

// 从站点移除指定回调（按 cb+user 匹配）。返回 0 = 成功，-1 = 没找到
int ii_remove_callback(unsigned long addr, ii_callback cb, void* user);

// 某站点上的回调数
int ii_callback_count(unsigned long addr);

// ---- 归属（模块分组）----
//
// 多个模块共用引擎时，各自只卸载自己的 hook：
//   ii_install_owned(addr, cb, user, "mod_a");
//   ii_uninstall_owner("mod_a");     // 只卸 mod_a 的
//
void ii_set_owner(unsigned long addr, const char* owner);
const char* ii_get_owner(unsigned long addr);
int  ii_uninstall_owner(const char* owner);   // 返回卸载数量

// 卸载
int ii_uninstall(unsigned long addr);

// 卸载全部
void ii_uninstall_all(void);

// 查询
int ii_count(void);
ii_site* ii_find(unsigned long addr);

// 判断某条指令能否插桩（是否支持重定位）
int ii_can_instrument(unsigned long addr);

// ---- handler 入口（供测试和自定义 trampoline 使用）----
//
// 正常使用时不用直接调：安装 hook 后由 shell 跳到这里。
// 导出是为了让 host 测试能验证「回调链是否真的被遍历」——
// 之前 ii_dispatch 读错的字段导致 FULL 模式回调从不执行，
// 而 host 测试只测了链表插入，没测它，所以漏了。
void* ii_dispatch(ii_ctx* c, uint64_t slotId);
void* ii_dispatch_light(ii_ctx_light* c, uint64_t slotId);

// ---- 仅供测试：注册一个「不真正插桩」的假站点 ----
//
// 目的：让 host 测试能验证 ii_dispatch 是否真的遍历回调链。
// 生产代码不应调用。
//
// 返回 slotId（>=0），失败返回负数。
int ii_test_register_site(unsigned long fakeAddr, ii_callback cb, void* user, int priority);
// 清理所有测试站点
void ii_test_clear_sites(void);
// 测试用：给已注册站点加回调
int ii_test_add_cb(unsigned long fakeAddr, ii_callback cb, void* user, int priority);
// 测试用：把某站点标记为 dead（模拟 alive=0）
int ii_test_kill_cb(unsigned long fakeAddr, ii_callback cb);

// ---- 崩溃隔离 ----
//
// 默认不启用：setjmp 有 20-50ns 开销，高频函数上会累积。
// 需要「回调出错不带崩游戏」时显式用 ii_guard_call 包一层：
//
//   void my_cb(ii_ctx* c, void* u) { if (!ii_guard_call(real_cb, c, u)) ... }
//
// 或直接用 ii_hook_guarded()（门面层提供）。
//
// 返回 1 = 回调正常返回；0 = 回调崩溃，已被隔离
int  ii_guard_call(ii_callback cb, ii_ctx* c, void* user);
long ii_guard_caught(void);         // 已隔离的崩溃次数

// ---------------------------------------------------------------------------
// 按需启停（性能优化用）
//
//   ii_set_enabled(addr, 0)  标记为待禁用（不立即动代码）
//   ii_set_enabled(addr, 1)  标记为待启用
//   ii_apply_pending()       一次性批量应用：没有待处理项时零开销
//
// 典型的性能优化用法：
//   进入战斗 → 批量 set_enabled(sites, 1) → apply_pending()
//   离开战斗 → 批量 set_enabled(sites, 0) → apply_pending()
//
// 禁用只恢复原指令，站点记录保留，可以随时重新启用。
// ---------------------------------------------------------------------------
int ii_set_enabled(unsigned long addr, int on);
int ii_is_enabled(unsigned long addr);
int ii_apply_pending(void);          // 返回实际处理的站点数
int ii_pending_count(void);

// 把全部站点设为某个状态（不立即应用）
int ii_set_enabled_all(int on);

// ---------------------------------------------------------------------------
// cancel（对应 Mixin 的 ci.cancel()）
//
// 在回调里调用 ii_cancel(c) 请求取消：函数立即返回，不等剩余逻辑执行。
// 返回值放在 c->x[0]（整型）或 c->s[0]（浮点）—— 注意这里没有 s 数组，
// 浮点返回值要用 ii_set_ret_float()。
//
// ★ 只有「函数入口」的站点能 cancel。函数中间 cancel 会漏掉 callee-saved
//   恢复和栈帧平衡，所以必须先用 ii_set_cancelable(addr, 1) 显式开启。
// ---------------------------------------------------------------------------
void ii_cancel(ii_ctx* c);
int  ii_set_cancelable(unsigned long addr, int on);

// ---------------------------------------------------------------------------
// 返回值类型校验（对应 Mixin 的编译期类型检查）
//
// 我们拿不到 C 的类型信息，但 rettype_db.h 里有 124/124 的人工标注 + 采样验证。
// 这些 API 让回调能在设置返回值前确认「这个函数到底返回什么」，
// 避免把 int 写到 float 返回的函数上（写错寄存器，静默出错）。
//
// 返回值为 ZR_* 常量（见 rettype_db.h）：0=UNKNOWN 1=VOID 2=BOOL
//                                            3=INT  4=FLOAT 5=PTR 6=STRUCT
// ---------------------------------------------------------------------------
#define II_RET_UNKNOWN 0
#define II_RET_VOID    1
#define II_RET_BOOL    2
#define II_RET_INT     3
#define II_RET_FLOAT   4
#define II_RET_PTR     5
#define II_RET_STRUCT  6

// 按站点的绝对地址查（要求该站点由本引擎安装）
int  ii_ret_kind(unsigned long addr);
// 按函数名查（名字来自 zinfo_db）
int  ii_ret_kind_name(const char* funcName);
// 按地址查可读类型名，如 "ItemStack*" / "float" / "void"
const char* ii_ret_type_name(unsigned long addr);
// 该函数的返回值应该写哪个寄存器：0=无需 1=x0 2=s0
int  ii_ret_reg(unsigned long addr);

// ---------------------------------------------------------------------------
// 字段访问器（对应 Mixin 的 @Accessor）
//
// fields_real.h 里有 716 条「某函数访问了某对象的 +N 偏移，宽 size 字节」的
// 静态分析结果。这里把它变成可用 API：
//
//   void cb(ii_ctx* c, void* u) {
//       // 从第 0 个参数(this) 的 +0x2a0 读 4 字节
//       int64_t v = ii_field_read(c, 0, 0x2a0, 4);
//       ii_field_write(c, 0, 0x2a0, 4, 100);
//   }
//
// 注意：只能访问静态分析覆盖到的「函数 × 偏移」组合。要凭空读任意对象的
// 任意字段，需要完整的类布局 —— 那和 @Local 一样，是拿不到的。
// ---------------------------------------------------------------------------
int64_t ii_field_read(ii_ctx* c, uint8_t argIdx, uint16_t off, uint8_t size);
void    ii_field_write(ii_ctx* c, uint8_t argIdx, uint16_t off, uint8_t size, int64_t v);

// 查某函数访问过的字段个数（funcName 来自 zinfo_db）
int ii_field_count(const char* funcName);
// 取第 idx 条字段记录的描述；任一出参可为 NULL。返回 0=成功
int ii_field_at(const char* funcName, int idx,
                uint16_t* offOut, uint8_t* baseOut, uint8_t* sizeOut, uint8_t* wrOut);

// ---------------------------------------------------------------------------
// 回调优先级（对应 Mixin 的 priority）
//
// 同一站点上多个探针的回调按 priority 降序执行（大的先跑）。
// 相同 priority 按注册顺序。用于「A 探针要先把参数改掉，B 探针再读」这类场景。
// 总登记数（含自己）走 ii_site_priority() 查。
// ---------------------------------------------------------------------------
#define II_PRIO_DEFAULT   0
#define II_PRIO_LOW     (-100)
#define II_PRIO_HIGH     100

// 带优先级的安装（旧 ii_install/ii_install_light 等价于 priority=II_PRIO_DEFAULT）
int ii_install_ex(unsigned long addr, ii_callback cb, void* user, int priority);
int ii_install_light_ex(unsigned long addr, ii_light_callback cb, void* user, int priority);
// 查/改某站点的优先级（只影响它自己是第一个安装者的情况）
int ii_site_priority(unsigned long addr);
int ii_set_site_priority(unsigned long addr, int priority);

// ---------------------------------------------------------------------------
// 函数内调用点（B3，部分对标 Mixin @Redirect）
//
// blcall_db.h 里有 449 条「某函数内部的某次调用」的静态分析结果。
//
// ★ 能力边界必须说清：
//   bl 指令本身不能直接插桩（trampoline 无法搬移跳转指令，ii_can_instrument 会拒绝）。
//   所以这里提供的是「调用点地址查询 + 调用【前】一条指令插桩」，
//   属于**观察**能力 —— 你能知道某函数即将调用谁、此时参数是什么。
//
//   真正的 @Redirect（把这次调用换成别的函数）需要新的 shell 类型：
//   把 bl 改成 blr 到任意目标 + 完成后再回到 callSite+4，未实现。
// ---------------------------------------------------------------------------
// 查调用点绝对地址（nth 从 0 开始计同名同目标的第几个）
int ii_call_site_addr(const char* funcName, const char* targetName,
                      int nth, unsigned long* outAddr);
// 某函数内调用某目标的调用点个数
int ii_call_site_count(const char* funcName, const char* targetName);
// 在调用点的【前一条指令】装桩（观察用）。返回 0 成功。
int ii_install_before_call(const char* funcName, const char* targetName,
                           int nth, ii_callback cb, void* user);
// 表内总条目数
int ii_call_site_total(void);

// ---------------------------------------------------------------------------
// 调用重定向（完整对标 Mixin 的 @Redirect）
//
// 把「某函数内部对 X 的一次调用」整个换掉：回调可以返回另一个函数地址，
// 那次调用就去调它；返回 NULL 则调原目标。
//
// 机制：
//   原:  callSite: bl origTarget
//   改:  callSite: b shell            ← 4 字节原子写，±128MB
//   shell → ii_redirect_common：
//     保存 x0-x7 / q0-q7（参数与浮点参数）
//     调 ii_redirect_dispatch 让回调决定目标（可在 ctx 里改参数）
//     恢复参数，设 x30 = callSite+4，br 目标
//   目标函数 ret 时 x30 正好是 callSite+4 —— 与原 bl 的行为完全一致。
//
// 回调签名：返回非 NULL = 改调该地址；返回 NULL = 调原目标。
//
// ★ 返回的地址必须是可执行内存。引擎不做校验（每次调用都查会拖慢热路径），
//   给错地址会直接跳到非法内存 —— 这是刻意的快速失败，不要指望兜底。
// ---------------------------------------------------------------------------
typedef void* (*ii_redirect_cb)(ii_ctx* c, void* user);

int ii_redirect_call(unsigned long callSiteAddr, unsigned long origTarget,
                     ii_redirect_cb cb, void* user);
// 查某重定向站点原本要调的目标（回调里常用：条件性转发回原函数）
unsigned long ii_redirect_orig_target(unsigned long callSiteAddr);

// 按「宿主函数名 + 目标函数名 + 第几个」直接安装（用 blcall_db 查地址）
int ii_redirect_name(const char* hostFunc, const char* targetFunc, int nth,
                     ii_redirect_cb cb, void* user);

// ---------------------------------------------------------------------------
// 常量修改（@ModifyConstant 等价）
//
// const_db.h 里有 26,447 条「函数内某条指令编码的立即数」（cmp / mov / add-sub）。
// 在这些指令处插桩，回调里改对应寄存器即可改变比较/赋值用的常量。
// ---------------------------------------------------------------------------
// 某函数内等于 value 的常量出现次数
int ii_const_count(const char* funcName, uint32_t value);
// 第 nth 个的绝对指令地址
int ii_const_addr(const char* funcName, uint32_t value, int nth, unsigned long* outAddr);
// 在常量指令处插桩（回调可改寄存器）
int ii_install_at_const(const char* funcName, uint32_t value, int nth,
                        ii_callback cb, void* user);

// ---------------------------------------------------------------------------
// 返回点 / 函数末尾（@Inject(RETURN) / @Inject(TAIL) 等价）
//
// ret_db.h 里有 2,754 条 ret 指令（多返回路径全部枚举）。
// ---------------------------------------------------------------------------
int ii_retpoint_count(const char* funcName);
int ii_retpoint_addr(const char* funcName, int nth, unsigned long* outAddr);
// 在返回点【前一条指令】插桩（返回值此时已在 x0/s0 里，可读可改）
int ii_install_at_return(const char* funcName, int nth, ii_callback cb, void* user);
// 在函数末尾（最后一个 ret 之前）插桩 = TAIL
int ii_install_at_tail(const char* funcName, ii_callback cb, void* user);
int ii_retpoint_total(void);

// ---------------------------------------------------------------------------
// 参数类型推断（补上「PTR 到底是什么」）
//
// rettype_db 的返回值类型已经精确（"ItemStack*" 而非 "PTR"），
// 但参数只有 PTR/INT/FLOAT 的粗分类。这里用两种信息推断具体类名：
//   1) 函数名的「所属类」前缀：Actor_getHealth → this 很可能是 Actor*
//   2) 已知函数作为目标时，调用方传进去的对象类
//
// 拿不准的一律返回 "ptr"，绝不编造 —— 错的类型名比没有更危险。
// ---------------------------------------------------------------------------
// 从函数名推断所属类（"Actor_getHealth" → "Actor"）
const char* ii_owner_class(const char* funcName);
// 第 argIdx 个参数的类型名（"Actor*" / "int" / "float" / "ptr"）
const char* ii_arg_type(const char* funcName, int argIdx);

// ---------------------------------------------------------------------------
// 崩溃现场记录（改善调试体验）
//
// Mixin 出错会给出带行号的 MixinApplyError；我们崩了只有 tombstone 里那 30 个寄存器。
// 这里维护一个环形缓冲，记录最近 N 次插桩命中（站点 / pc / 时间），
// 崩溃时就能回答「死之前最后跑的是哪个站点」。
//
// 用法：
//     ii_trace_begin();          // 开始记录（有开销，只在调试期开）
//     ...跑游戏...
//     ii_trace_dump();           // 手动转储当前缓冲
//     ii_trace_install_crash_handler();   // 可选：崩在 SIGSEGV/SIGABRT 时自动转储
//
// 注意：crash handler 会覆盖进程原有的 SIGSEGV 处理（游戏的完整性校验可能也装了）。
//       只在排查问题时临时启用。
// ---------------------------------------------------------------------------
#define II_TRACE_N 64

typedef struct {
    uint64_t siteId;    // 站点号
    uint64_t addr;      // 被插桩的指令地址
    uint64_t t_ms;      // 单调时钟毫秒
    uint32_t seq;       // 命中序号
    uint32_t pad;
} ii_trace_rec;

void ii_trace_begin(void);
void ii_trace_end(void);
void ii_trace_dump(void);                 // 转储到日志
int  ii_trace_read(int idx, ii_trace_rec* out);   // 读缓冲（idx 0 = 最近）
int  ii_trace_install_crash_handler(void);
int  ii_trace_is_on(void);

// ---------------------------------------------------------------------------
// 局部变量访问（@Local 的部分替代）
//
// frame_db.h 里有 120 个函数的栈帧信息 + 547 个局部变量槽位（[sp, #off] 的 store）。
//
// 能做到：
//     int64_t v = ii_local_read(c, spOff, size);      // 读 c->sp + spOff
//     ii_local_write(c, spOff, size, v);
//     ii_frame_size(funcName);                         // 栈帧大小
//     ii_local_slot(funcName, idx, &spOff, &size, ...)  // 枚举槽位
//
// 做不到（与 Mixin 的 @Local 的本质差距）：
//     不知道哪个槽是哪个变量、什么类型。arm64 机器码没有 LocalVariableTable，
//     这一层信息补不回来。用之前必须看反汇编确认语义。
//
// ★ 注意 c->sp 是「被插桩指令执行时的 sp」：
//     插桩点在函数体中部（sub sp,sp,#N 之后）→ c->sp + spOff 正确
//     插桩点在函数入口（sub sp 之前）        → 需自行减 frameSize
// ---------------------------------------------------------------------------
int64_t ii_local_read(ii_ctx* c, uint16_t spOff, uint8_t size);
void    ii_local_write(ii_ctx* c, uint16_t spOff, uint8_t size, int64_t v);
// --- 精确栈帧（来自 .eh_frame CFI，比静态扫描准得多）---
// frame.cfi.bin 由 parse_ehframe.py + gen_frame_cfi.py 生成，运行时 mmap 加载。
int  ii_frame_cfi_load(const char* path);
int  ii_frame_cfi_loaded(void);
// 按函数 vaddr 查：cfaReg 31=sp 29=x29；frameSize 就是 cfaOff
int  ii_frame_cfi(uint32_t funcOff, uint8_t* cfaRegOut, int16_t* cfaOffOut, uint8_t* nregOut);

int     ii_frame_size(const char* funcName);
int     ii_local_count(const char* funcName);
int     ii_local_slot(const char* funcName, int idx,
                      uint16_t* spOffOut, uint8_t* sizeOut, uint32_t* insnOffOut);

// 设置返回值（配合 ii_cancel 使用）
static inline void ii_set_ret_int(ii_ctx* c, int64_t v)   { c->x[0] = (uint64_t)v; }
static inline void ii_set_ret_ptr(ii_ctx* c, void* v)     { c->x[0] = (uint64_t)v; }
static inline void ii_set_ret_float(ii_ctx* c, float v) {
    uint32_t bits; __builtin_memcpy(&bits, &v, 4);
    c->q[0] = (__uint128_t)bits;     // s0 是 q0 的低 32 位
}

// ===========================================================================
// 引擎实例（缺陷 #1：从全局单例改为可实例化）
//
// 默认实例 ii_default() 保留原有的全局语义，所以老代码不用改：
//   ii_install(addr, cb, user)          ← 走默认实例
//   ii_engine_install(e, addr, cb, user) ← 走指定实例
//
// 实例之间完全隔离：站点表、追踪缓冲、CFI、池统计各自独立。
// 进程级的东西（跨 .so 共享区、看门狗）仍只有一份 —— 那本来就该全局。
// ===========================================================================
typedef struct ii_engine ii_engine;

// 创建/销毁实例（ii_engine_destroy 会卸载该实例下所有站点）
ii_engine* ii_engine_create(void);
void       ii_engine_destroy(ii_engine* e);

// 默认实例（进程内唯一，第一次调用时自动创建）
ii_engine* ii_default(void);

// 实例上操作（语义与同名全局 API 完全一致）
int  ii_engine_install(ii_engine* e, unsigned long addr, ii_callback cb, void* user);
int  ii_engine_install_ex(ii_engine* e, unsigned long addr, ii_callback cb, void* user, int priority);
int  ii_engine_uninstall(ii_engine* e, unsigned long addr);
void ii_engine_uninstall_all(ii_engine* e);
int  ii_engine_count(ii_engine* e);
ii_site* ii_engine_find(ii_engine* e, unsigned long addr);
int  ii_engine_trace_begin(ii_engine* e);
int  ii_engine_trace_end(ii_engine* e);
int  ii_engine_trace_read(ii_engine* e, int idx, ii_trace_rec* out);
int  ii_engine_trace_dump(ii_engine* e);
int  ii_engine_frame_cfi_load(ii_engine* e, const char* path);
int  ii_engine_frame_cfi_loaded(ii_engine* e);

// 实例统计
typedef struct {
    int      nSites;
    int      capacity;
    uint32_t traceSeq;
    int      traceOn;
    long     poolDeaths;
    long     poolRecoveries;
    int      cfiLoaded;
} ii_engine_stats;

int ii_engine_get_stats(ii_engine* e, ii_engine_stats* out);

// ---------------------------------------------------------------------------
// 回调崩溃隔离（底层 API）
//
// 回调里访问野指针会让整个进程 SIGSEGV。这里在回调执行期间临时接管
// SIGSEGV/SIGBUS，出错时 siglongjmp 回来，把"回调崩了"变成"回调返回失败"。
//
// 代价：setjmp 约 20-50 ns，所以做成显式启用（ii_guard_call）而非默认。
// 上层封装见 zh_hook_guarded / zh_guard_caught。
// ---------------------------------------------------------------------------
int  ii_guard_call(ii_callback cb, ii_ctx* c, void* user);
long ii_guard_caught(void);

#ifdef __cplusplus
}
#endif
