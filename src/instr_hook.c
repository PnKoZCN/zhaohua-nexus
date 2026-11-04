#include "zh/instr_hook.h"
#include "common/crashsafe.h"
#include "zh/reloc.h"
#include "rettype_db.h"
#include "fields_real.h"
#include "blcall_db.h"
#include "const_db.h"
#include "ret_db.h"
#include "frame_db.h"
#include "zh/zhreg.h"
#include "zh_tbl.h"
#include "zh/zlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <signal.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <sched.h>
// futex 常量 —— 不依赖 <linux/futex.h>（部分环境没有该头文件）
// 值来自内核 UAPI，稳定不变
#ifndef FUTEX_WAIT
#define FUTEX_WAIT            0
#endif
#ifndef FUTEX_WAKE
#define FUTEX_WAKE            1
#endif
#ifndef FUTEX_PRIVATE_FLAG
#define FUTEX_PRIVATE_FLAG    128
#endif
#ifndef FUTEX_WAIT_PRIVATE
#define FUTEX_WAIT_PRIVATE    (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#endif
#ifndef FUTEX_WAKE_PRIVATE
#define FUTEX_WAKE_PRIVATE    (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#endif
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdarg.h>

// ============================================================================
// 栈布局（与 ii_ctx 完全一致，800 字节 = 0x320）
//   sp+0x000 : x0..x30   (31 × 8 = 248)
//   sp+0x0f8 : sp
//   sp+0x100 : pc（调度后改用来存 trampoline 地址）
//   sp+0x108 : pstate(nzcv)
//   sp+0x110 : q0..q31   (32 × 16 = 512)
//   sp+0x310 : fpsr
//   sp+0x318 : fpcr
//
// 寄存器牺牲策略：
//   x16/x17 是 AAPCS64 的 IP0/IP1（intra-procedure-call scratch），
//   编译器本就不假设它们跨调用保值，所以用它们传递
//   「trampoline 地址」和「原 sp」。其余 29 个寄存器精确恢复。
//   ii_can_instrument() 会拒绝那些自身写 x16/x17 的指令。
// ============================================================================
#define CTX_SIZE   0x320
#define OFF_SP     0x0f8
#define OFF_PC     0x100
#define OFF_PSTATE 0x108

#if ZH_ARCH_ARM64
__asm__(
    ".text\n"
    ".global ii_common\n"
    ".type ii_common, %function\n"
    "ii_common:\n"
    /* 进入时：x17 = 站点号（shell 设的）
       [sp] = 原 x16，[sp+8] = 原 x17（shell 压的）
       注意 x17 此刻是站点号，不是原值 —— 后面 dispatch 要用它 */
    "  sub  sp, sp, #0x320\n"
    "  stp  x0,  x1,  [sp, #0x000]\n"
    "  stp  x2,  x3,  [sp, #0x010]\n"
    "  stp  x4,  x5,  [sp, #0x020]\n"
    "  stp  x6,  x7,  [sp, #0x030]\n"
    "  stp  x8,  x9,  [sp, #0x040]\n"
    "  stp  x10, x11, [sp, #0x050]\n"
    "  stp  x12, x13, [sp, #0x060]\n"
    "  stp  x14, x15, [sp, #0x070]\n"
    "  stp  x16, x17, [sp, #0x080]\n"
    "  stp  x18, x19, [sp, #0x090]\n"
    "  stp  x20, x21, [sp, #0x0a0]\n"
    "  stp  x22, x23, [sp, #0x0b0]\n"
    "  stp  x24, x25, [sp, #0x0c0]\n"
    "  stp  x26, x27, [sp, #0x0d0]\n"
    "  stp  x28, x29, [sp, #0x0e0]\n"
    "  str  x30,      [sp, #0x0f0]\n"
    /* 此刻 x16 = shell 跳板地址，x17 = 站点号，都不是原值。
       原 x16/x17 在 shell 压的栈上：[sp+0x320] / [sp+0x328]。
       搬进本帧 0x080/0x088 槽，trampoline 才能恢复出真值。
       x10 已在 [sp+0x050] 保存，可自由使用。 */
    "  ldr  x10,      [sp, #0x320]\n"
    "  str  x10,      [sp, #0x080]\n"
    "  ldr  x10,      [sp, #0x328]\n"
    "  str  x10,      [sp, #0x088]\n"
    "  add  x9, sp, #0x330\n"
    "  str  x9,       [sp, #0x0f8]\n"
    "  mrs  x9, nzcv\n"
    "  str  x9,       [sp, #0x108]\n"
    "  stp  q0,  q1,  [sp, #0x110]\n"
    "  stp  q2,  q3,  [sp, #0x130]\n"
    "  stp  q4,  q5,  [sp, #0x150]\n"
    "  stp  q6,  q7,  [sp, #0x170]\n"
    "  stp  q8,  q9,  [sp, #0x190]\n"
    "  stp  q10, q11, [sp, #0x1b0]\n"
    "  stp  q12, q13, [sp, #0x1d0]\n"
    "  stp  q14, q15, [sp, #0x1f0]\n"
    "  stp  q16, q17, [sp, #0x210]\n"
    "  stp  q18, q19, [sp, #0x230]\n"
    "  stp  q20, q21, [sp, #0x250]\n"
    "  stp  q22, q23, [sp, #0x270]\n"
    "  stp  q24, q25, [sp, #0x290]\n"
    "  stp  q26, q27, [sp, #0x2b0]\n"
    "  stp  q28, q29, [sp, #0x2d0]\n"
    "  stp  q30, q31, [sp, #0x2f0]\n"
    "  mrs  x9, fpsr\n"
    "  str  x9,       [sp, #0x310]\n"
    "  mrs  x9, fpcr\n"
    "  str  x9,       [sp, #0x318]\n"
    /* 调度：x0 = ctx(sp), x1 = slotId(x17) */
    "  mov  x0, sp\n"
    "  mov  x1, x17\n"      /* x17 = 站点号（shell 设的，未被覆盖） */
    "  bl   ii_dispatch\n"
    "  str  x0,       [sp, #0x100]\n"          /* 暂存 trampoline 到 pc 槽 */
    /* ---- 恢复 ---- */
    "  ldr  x9,       [sp, #0x310]\n"
    "  msr  fpsr, x9\n"
    "  ldr  x9,       [sp, #0x318]\n"
    "  msr  fpcr, x9\n"
    "  ldp  q30, q31, [sp, #0x2f0]\n"
    "  ldp  q28, q29, [sp, #0x2d0]\n"
    "  ldp  q26, q27, [sp, #0x2b0]\n"
    "  ldp  q24, q25, [sp, #0x290]\n"
    "  ldp  q22, q23, [sp, #0x270]\n"
    "  ldp  q20, q21, [sp, #0x250]\n"
    "  ldp  q18, q19, [sp, #0x230]\n"
    "  ldp  q16, q17, [sp, #0x210]\n"
    "  ldp  q14, q15, [sp, #0x1f0]\n"
    "  ldp  q12, q13, [sp, #0x1d0]\n"
    "  ldp  q10, q11, [sp, #0x1b0]\n"
    "  ldp  q8,  q9,  [sp, #0x190]\n"
    "  ldp  q6,  q7,  [sp, #0x170]\n"
    "  ldp  q4,  q5,  [sp, #0x150]\n"
    "  ldp  q2,  q3,  [sp, #0x130]\n"
    "  ldp  q0,  q1,  [sp, #0x110]\n"
    "  ldr  x9,       [sp, #0x108]\n"
    "  msr  nzcv, x9\n"
    /* x0-x15 精确恢复 */
    "  ldp  x0,  x1,  [sp, #0x000]\n"
    "  ldp  x2,  x3,  [sp, #0x010]\n"
    "  ldp  x4,  x5,  [sp, #0x020]\n"
    "  ldp  x6,  x7,  [sp, #0x030]\n"
    "  ldp  x8,  x9,  [sp, #0x040]\n"
    "  ldp  x10, x11, [sp, #0x050]\n"
    "  ldp  x12, x13, [sp, #0x060]\n"
    "  ldp  x14, x15, [sp, #0x070]\n"
    /* 跳过 0x080（x16/x17 牺牲） */
    "  ldp  x18, x19, [sp, #0x090]\n"
    "  ldp  x20, x21, [sp, #0x0a0]\n"
    "  ldp  x22, x23, [sp, #0x0b0]\n"
    "  ldp  x24, x25, [sp, #0x0c0]\n"
    "  ldp  x26, x27, [sp, #0x0d0]\n"
    "  ldp  x28, x29, [sp, #0x0e0]\n"
    "  ldr  x30,      [sp, #0x0f0]\n"
    /* 关键：不恢复 sp、不恢复 x16/x17 —— 交给 trampoline 收尾
       这样 x16/x17 的原值不会丢失（它们还在 handler 栈上） */
    "  ldr  x16,      [sp, #0x100]\n"   /* x16 = trampoline 地址 */
    /* ★ 兜底：trampoline 地址为 0 时绝不能 br —— 会跳到地址 0 直接闪退。
       ii_dispatch 在站点已被卸载（sid 对不上表）时返回 NULL，
       这个 shell 却还挂在原函数入口上，被调用到就会走到这里。
       退化成"恢复现场后直接返回"：x0-x30 已恢复，sp 也已恢复，
       此时 ret 会回到 lr（已被 ldr x30,[sp,#0xf0] 还原成调用者的返回地址）。 */
    "  cbz  x16, 9f\n"
    "  br   x16\n"
    "9:\n"
    "  ret\n"
);
#else
// 非 ARM64 平台：handler 无实际作用（引擎只在 ARM64 上做指令级插桩）。
// 提供桩实现让代码能在 x86_64 主机上构建并跑单元测试
// （数据表 / 编码器 / 链表等逻辑与架构无关，必须可测）。
void ii_common(void) { }
#endif


// ===========================================================================
// 轻量 handler —— 高频函数专用
//
// FULL handler 每次命中要 88 条指令、1096 字节内存流量（含 q0-q31）。
// 大多数 hook 场景只关心 x0-x7 和 s0-s7，走 LIGHT 可省 88%。
//
// LIGHT 的限制（必须让开发者知道）：
//   - 回调里只能读写 x0-x7 / s0-s7（其余寄存器不保存）
//   - 不能 cancel（没保存完整栈帧）
//   - 不能改 sp / pc
// ===========================================================================
// ===========================================================================
// 轻量 handler —— 高频函数专用
//
// FULL handler 每次命中 113 条指令、约 1100 字节内存流量（含 q0-q31）。
// 高频 hook（tick/渲染/碰撞）只关心 x0-x7 和 s0-s7，走 LIGHT 省 78%。
//
// 限制（API 层必须告知）：
//   - 回调只能读写 x0-x7 / s0-s7
//   - 不能 cancel
//   - 不能改 sp / pc / 其他寄存器
// ===========================================================================
// ===========================================================================
// 轻量 handler —— 高频函数专用
//
// FULL 每次命中 113 条指令 / 约 1100 字节内存流量（含 q0-q31）。
// 高频 hook 多数只关心 x0-x7 和 s0-s7，走 LIGHT 省 78%。
//
// ctx 布局（回调可见）：
//   0x00  x0..x7   (8 × 8B)
//   0x40  q0, q1   (2 × 16B)  ← q0 低 32 位 = s0，q0 高 32 位 = s1 ... q1 = s4..s7
// 内部数据放在 ctx 之后的 0x70 起。
//
// 限制：回调只能读写 x0-x7 / s0-s7；不能 cancel；不能改 sp/pc。
// ===========================================================================
// ===========================================================================
// 轻量 handler —— 比 FULL 少保存 callee-saved 部分
//
// 关键：bl 调用 C 函数后，按 AAPCS64，C 函数可以随意破坏：
//   x0-x18（caller-saved）
//   q0-q7, q16-q31（d0-d7, d16-d31）
// 但保证保护：
//   x19-x28, q8-q15（d8-d15）
//
// 所以 handler 必须保存全部 caller-saved，否则函数后续指令会读到垃圾。
// （第一版只保存 x0-x7 + q0-q1，导致 CameraAPI_getFov 跑了 1106 帧后崩溃。）
//
// ctx 布局保持与 ii_ctx_light 兼容：
//   0x00  x0-x7     ← 回调可见
//   0x40  q0,q1     ← s0-s7
// ===========================================================================
// ===========================================================================
// 轻量 handler
//
// 必须保存全部 caller-saved（bl 调 C 函数后 x0-x18 / q0-q7 / q16-q31 全被破坏），
// callee-saved（x19-x28 / q8-q15）由 C 函数保证保护，不必保存。
//
// 栈布局（stp/ldp qN,qM 的立即数必须是 16 的倍数！）：
//   0x000 x0,x1    0x040 q0,q1  ← ctx 兼容区（ii_ctx_light: x[8] + q[2]）
//   0x010 x2,x3    0x060 x8,x9
//   0x020 x4,x5    0x070 x10,x11
//   0x030 x6,x7    0x080 x12,x13
//                  0x090 x14,x15
//                  0x0a0 x16,x17
//                  0x0b0 x18,x30
//                  0x0c0 原x16 / 原x17
//                  0x0d0 tramp / pad
//                  0x0e0 q2,q3   0x100 q4,q5   0x120 q6,q7
//                  0x140 q16,q17 0x160 q18,q19 0x180 q20,q21
//                  0x1a0 q22,q23 0x1c0 q24,q25 0x1e0 q26,q27
//                  0x200 q28,q29 0x220 q30,q31
//   总共 0x240（576 字节）
//
// sp 平衡：handler 结束 add sp,sp,#0x250 —— 0x240 是自己分配的，
// 另外 0x10 是 shell 的 stp x16,x17,[sp,#-16]!。必须补回，
// 否则 trampoline 执行以 sp 为基址的原指令（如 str d10,[sp,#-0x40]!）会写错位置。
// ===========================================================================
#if ZH_ARCH_ARM64
__asm__(
    ".text\n"
    ".global ii_light\n"
    ".type ii_light, %function\n"
    "ii_light:\n"
    /* 进入：x17 = 站点号；[sp] = 原 x16，[sp+8] = 原 x17 */
    "  sub  sp, sp, #0x240\n"
    /* --- ctx 可见区 + 通用寄存器 --- */
    "  stp  x0,  x1,  [sp, #0x000]\n"
    "  stp  x2,  x3,  [sp, #0x010]\n"
    "  stp  x4,  x5,  [sp, #0x020]\n"
    "  stp  x6,  x7,  [sp, #0x030]\n"
    "  stp  q0,  q1,  [sp, #0x040]\n"
    "  stp  x8,  x9,  [sp, #0x060]\n"
    "  stp  x10, x11, [sp, #0x070]\n"
    "  stp  x12, x13, [sp, #0x080]\n"
    "  stp  x14, x15, [sp, #0x090]\n"
    "  stp  x16, x17, [sp, #0x0a0]\n"
    "  stp  x18, x30, [sp, #0x0b0]\n"
    /* shell 压栈处的原 x16 / 原 x17 */
    "  ldr  x9,       [sp, #0x240]\n"   /* 单 ldr 上限 32760，ldp 只有 504 */
    "  ldr  x10,      [sp, #0x248]\n"
    "  stp  x9,  x10, [sp, #0x0c0]\n"
    /* --- caller-saved SIMD（全部 16 对齐）--- */
    "  stp  q2,  q3,  [sp, #0x0e0]\n"
    "  stp  q4,  q5,  [sp, #0x100]\n"
    "  stp  q6,  q7,  [sp, #0x120]\n"
    "  stp  q16, q17, [sp, #0x140]\n"
    "  stp  q18, q19, [sp, #0x160]\n"
    "  stp  q20, q21, [sp, #0x180]\n"
    "  stp  q22, q23, [sp, #0x1a0]\n"
    "  stp  q24, q25, [sp, #0x1c0]\n"
    "  stp  q26, q27, [sp, #0x1e0]\n"
    "  stp  q28, q29, [sp, #0x200]\n"
    "  stp  q30, q31, [sp, #0x220]\n"
    /* --- 调度 --- */
    "  mov  x0, sp\n"
    "  mov  x1, x17\n"
    "  bl   ii_dispatch_light\n"
    "  str  x0,       [sp, #0x0d0]\n"     /* 暂存 trampoline */
    /* --- 恢复（先读后改 sp）--- */
    "  ldp  q30, q31, [sp, #0x220]\n"
    "  ldp  q28, q29, [sp, #0x200]\n"
    "  ldp  q26, q27, [sp, #0x1e0]\n"
    "  ldp  q24, q25, [sp, #0x1c0]\n"
    "  ldp  q22, q23, [sp, #0x1a0]\n"
    "  ldp  q20, q21, [sp, #0x180]\n"
    "  ldp  q18, q19, [sp, #0x160]\n"
    "  ldp  q16, q17, [sp, #0x140]\n"
    "  ldp  q6,  q7,  [sp, #0x120]\n"
    "  ldp  q4,  q5,  [sp, #0x100]\n"
    "  ldp  q2,  q3,  [sp, #0x0e0]\n"
    "  ldp  q0,  q1,  [sp, #0x040]\n"
    "  ldp  x8,  x9,  [sp, #0x060]\n"
    "  ldp  x10, x11, [sp, #0x070]\n"
    "  ldp  x12, x13, [sp, #0x080]\n"
    "  ldp  x14, x15, [sp, #0x090]\n"
    "  ldr  x18,      [sp, #0x0b0]\n"
    "  ldr  x30,      [sp, #0x0b8]\n"
    "  ldp  x0,  x1,  [sp, #0x000]\n"
    "  ldp  x2,  x3,  [sp, #0x010]\n"
    "  ldp  x4,  x5,  [sp, #0x020]\n"
    "  ldp  x6,  x7,  [sp, #0x030]\n"
    "  ldr  x16,      [sp, #0x0c0]\n"
    "  ldr  x17,      [sp, #0x0c8]\n"
    "  ldr  x9,       [sp, #0x0d0]\n"     /* trampoline */
    "  add  sp, sp, #0x250\n"             /* 0x240 自己分配 + 0x10 shell 压的 */
    "  br   x9\n"
);
#else
// 非 ARM64 平台：handler 无实际作用（引擎只在 ARM64 上做指令级插桩）。
// 提供桩实现让代码能在 x86_64 主机上构建并跑单元测试
// （数据表 / 编码器 / 链表等逻辑与架构无关，必须可测）。
void ii_light(void) { }
#endif
extern void ii_light(void);

// ===========================================================================
// 调用重定向的 handler
//
// 进入时：x17 = 站点号；[sp] = 原 x16，[sp+8] = 原 x17（shell 压的）
// 出口：直接 br 到目标函数，x30 = callSite+4（模拟 bl 的效果）
//
// 栈布局（0x120）：
//   0x000 x0,x1   0x010 x2,x3   0x020 x4,x5   0x030 x6,x7
//   0x040 q0,q1   0x060 q2,q3   0x080 q4,q5   0x0a0 q6,q7
//   0x0c0 暂存目标地址
//   0x100 pc 槽（ctx 布局，dispatch 把 callSite+4 写这里）
// ===========================================================================
#if ZH_ARCH_ARM64
__asm__(
    ".text\n"
    ".global ii_redirect_common\n"
    ".type ii_redirect_common, %function\n"
    "ii_redirect_common:\n"
    /*
     * 栈帧必须与 ii_ctx 的真实布局严格对应：
     *   0x000  x[31]     248 字节（x0..x30）
     *   0x0F8  sp
     *   0x100  pc
     *   0x108  pstate
     *   0x110  q[32]     512 字节（q0..q31）
     * 总共 0x310 字节，向上取整到 0x320，再加 16 字节存跳转目标。
     *
     * 旧版本把 q0-q7 写到 [sp,#0x40..0xa0]，那是 x[8]..x[20] 的地盘，
     * 等于把 x 寄存器的值覆盖成向量寄存器内容 —— 回调拿到的参数全是垃圾，
     * 而且栈只减 0x120 根本放不下 q 数组（真机表现：装好后几秒卡死闪退）。
     */
    "  sub  sp,  sp,  #0x330\n"          /* 0x320 (ctx) + 0x10 (对齐/暂存) */
    "  stp  x0,  x1,  [sp, #0x000]\n"
    "  stp  x2,  x3,  [sp, #0x010]\n"
    "  stp  x4,  x5,  [sp, #0x020]\n"
    "  stp  x6,  x7,  [sp, #0x030]\n"
    "  stp  x8,  x9,  [sp, #0x040]\n"
    "  stp  x10, x11, [sp, #0x050]\n"
    "  stp  x12, x13, [sp, #0x060]\n"
    "  stp  x14, x15, [sp, #0x070]\n"
    /* x16/x17 在 0x080/0x088：虽然 ABI 允许破坏，但回调通过 ctx->x[16/17]
       读到的必须是真实值，否则拿到未初始化内存。x17 就是 slotId，也一并存。 */
    "  stp  x16, x17, [sp, #0x080]\n"
    "  stp  x18, x19, [sp, #0x090]\n"
    "  stp  x20, x21, [sp, #0x0A0]\n"
    "  stp  x22, x23, [sp, #0x0B0]\n"
    "  stp  x24, x25, [sp, #0x0C0]\n"
    "  stp  x26, x27, [sp, #0x0D0]\n"
    "  stp  x28, x29, [sp, #0x0E0]\n"
    "  str  x30,      [sp, #0x0F0]\n"    /* x30 (lr) */
    /* ctx.sp 在 0x0F8：填"调用前的 sp" = 当前 sp + 0x330 + 16（shell 压的） */
    "  add  x9,  sp,  #0x340\n"
    "  str  x9,       [sp, #0x0F8]\n"
    /* ctx.pc 在 0x100：dispatch 会写，先填 0 */
    "  str  xzr,      [sp, #0x100]\n"
    /* ctx.pstate 在 0x108 */
    "  mrs  x9,  nzcv\n"
    "  str  x9,       [sp, #0x108]\n"
    /* ctx.q[0..7] 在 0x110 起 */
    "  stp  q0,  q1,  [sp, #0x110]\n"
    "  stp  q2,  q3,  [sp, #0x130]\n"
    "  stp  q4,  q5,  [sp, #0x150]\n"
    "  stp  q6,  q7,  [sp, #0x170]\n"
    /* 调 dispatch(ctx, sid) */
    "  mov  x0,  sp\n"
    "  mov  x1,  x17\n"
    "  bl   ii_redirect_dispatch\n"
    "  str  x0,       [sp, #0x320]\n"    /* 暂存跳转目标 */
    /* 恢复 q0-q7（回调可能改过） */
    "  ldp  q6,  q7,  [sp, #0x170]\n"
    "  ldp  q4,  q5,  [sp, #0x150]\n"
    "  ldp  q2,  q3,  [sp, #0x130]\n"
    "  ldp  q0,  q1,  [sp, #0x110]\n"
    /* 恢复 x0-x15、x18-x29（回调可能改过参数；x16/x17 是临时值，不恢复） */
    "  ldp  x28, x29, [sp, #0x0E0]\n"
    "  ldp  x26, x27, [sp, #0x0D0]\n"
    "  ldp  x24, x25, [sp, #0x0C0]\n"
    "  ldp  x22, x23, [sp, #0x0B0]\n"
    "  ldp  x20, x21, [sp, #0x0A0]\n"
    "  ldp  x18, x19, [sp, #0x090]\n"
    "  ldp  x14, x15, [sp, #0x070]\n"
    "  ldp  x12, x13, [sp, #0x060]\n"
    "  ldp  x10, x11, [sp, #0x050]\n"
    "  ldp  x8,  x9,  [sp, #0x040]\n"
    "  ldp  x6,  x7,  [sp, #0x030]\n"
    "  ldp  x4,  x5,  [sp, #0x020]\n"
    "  ldp  x2,  x3,  [sp, #0x010]\n"
    "  ldp  x0,  x1,  [sp, #0x000]\n"
    /* 取跳转目标和返回地址 */
    "  ldr  x16,      [sp, #0x320]\n"
    "  ldr  x9,       [sp, #0x100]\n"    /* ctx.pc = callSite+4 */
    "  mov  x30, x9\n"
    "  add  sp,  sp,  #0x330\n"
    "  add  sp,  sp,  #0x10\n"           /* 补回 shell 压的 16 字节 */
    /* 目标为 0 时绝不能 br —— 会跳到地址 0 直接闪退 */
    "  cbz  x16, 1f\n"
    "  br   x16\n"
    "1:\n"
    "  ret\n"
);
#else
// 非 ARM64 平台：handler 无实际作用（引擎只在 ARM64 上做指令级插桩）。
// 提供桩实现让代码能在 x86_64 主机上构建并跑单元测试
// （数据表 / 编码器 / 链表等逻辑与架构无关，必须可测）。
void ii_redirect_common(void) { }
#endif
extern void ii_redirect_common(void);

extern void ii_common(void);
extern void ii_cancel_tramp(void);   // cancel：恢复 x16/x17 与 sp 后 ret

// cancel 跳板：从 handler 栈恢复牺牲寄存器、平衡栈帧、返回调用者。
// 只有「函数入口」的站点能走这里 —— 函数中间 cancel 会漏掉 callee-saved 恢复。
#if ZH_ARCH_ARM64
__asm__(
    ".text\n"
    ".global ii_cancel_tramp\n"
    ".type ii_cancel_tramp, %function\n"
    "ii_cancel_tramp:\n"
    "  ldp  x16, x17, [sp, #0x080]\n"   /* 恢复 shell 牺牲的 x16/x17 */
    "  add  sp, sp, #0x330\n"           /* 恢复插桩点时的 sp */
    "  ret\n"                            /* 跳到 x30（入口=调用者返回地址） */
);
#else
// 非 ARM64 平台：handler 无实际作用（引擎只在 ARM64 上做指令级插桩）。
// 提供桩实现让代码能在 x86_64 主机上构建并跑单元测试
// （数据表 / 编码器 / 链表等逻辑与架构无关，必须可测）。
void ii_cancel_tramp(void) { }
#endif

// ===========================================================================
// 引擎实例
//
// 原来这些是散落的全局变量，现在收进结构体。
// 默认实例 g_defaultEngine 预置，所以老代码（全局 API）行为完全不变。
// ===========================================================================

struct ii_engine {
    // ---- 站点表 ----
    // ★ 重构：站点表全局唯一（只有 g_defaultEngine 用）。
    //   多实例不再各自持有表 —— 因为 shell 里硬编码的 slotId 是
    //   "全局表索引"，一旦 dispatch 在热路径上按别的实例查表，
    //   site_at() 就返回 NULL，回调永远不执行（t11 的真机现象）。
    //   非默认实例的 siteChunk 恒为 NULL，nSites 只做归属计数。
    ii_site** siteChunk[II_SITE_CHUNKS];
    int       nChunk;
    int       nSites;             // 默认实例=全局站点数；其他实例=自己装的站点数
    unsigned long ownAddr[II_ENGINE_MAX_OWN];   // 本实例装的地址（用于分组卸载）
    int       nOwn;

    // ---- 追踪 ----
    ii_trace_rec      trace[II_TRACE_N];
    volatile int      traceOn;
    volatile uint32_t traceSeq;

    // ---- 统计 ----
    volatile long poolDeaths;
    volatile long poolRecoveries;

    // ---- CFI ----
    const uint8_t* cfiBase;
    size_t         cfiLen;
    uint32_t       cfiCount;

    // ---- BSS 池扫描标记 ----
    int bssScanned;

    // ---- 跨探针共享表里的身份 ----
    // 0 表示"用进程级 g_myId"（默认实例）。
    // 每个 ii_engine_create() 的实例分到独立 id，这样同一进程内多个
    // 实例挂在同一地址时，dispatch 不会因为 owner==g_myId 而把自己的
    // 回调全部跳过。
    int32_t ownerId;
};

// 默认实例（静态分配，避免第一个调用者承担 malloc）
static struct ii_engine g_defaultEngine;
static ii_engine*       g_defaultPtr = NULL;

ii_engine* ii_default(void) {
    if (!g_defaultPtr) {
        memset(&g_defaultEngine, 0, sizeof g_defaultEngine);
        g_defaultPtr = &g_defaultEngine;
    }
    return g_defaultPtr;
}

// 实例 id 分配器。从 1000 起，避开跨探针表里其它探针占用的小 id。
static int32_t g_nextOwnerId = 1000;

ii_engine* ii_engine_create(void) {
    ii_engine* e = (ii_engine*)calloc(1, sizeof *e);
    if (!e) return NULL;
    // 独立身份：dispatch 时 owner != g_myId 才会执行外部回调，
    // 而同进程多实例必须靠这个字段区分彼此。
    e->ownerId = __atomic_fetch_add(&g_nextOwnerId, 1, __ATOMIC_RELAXED);
    return e;
}

// 前向声明（实现在 uninstall_all 之后）
void ii_engine_uninstall_all(ii_engine* e);
static void wd_log(const char* fmt, ...);   // 定义在下面，这里先用

void ii_engine_destroy(ii_engine* e) {
    if (!e || e == &g_defaultEngine) return;
    wd_log("[destroy] e=%p nSites=%d nOwn=%d ownerId=%d\n",
           (void*)e, e->nSites, e->nOwn, e->ownerId);

    // 只撤本实例自己装过的站点（ownAddr 里记的就是这些地址）。
    // 全局表在别的实例/全局引擎手里，绝不动它们。
    ii_engine_uninstall_all(e);

    // 私有站点表已废弃（结构里恒为 NULL），这里不再 free chunk。
    free(e);
    wd_log("[destroy] 完成\n");
}

// ---- 兼容层：老的全局变量名映射到默认实例 ----
// 保留这些宏，让 2,600 行既有代码不用逐行改
#define g_siteChunk      (ii_default()->siteChunk)
#define g_nChunk         (ii_default()->nChunk)
#define g_nSites         (ii_default()->nSites)
#define g_trace          (ii_default()->trace)
#define g_traceOn        (ii_default()->traceOn)
#define g_traceSeq       (ii_default()->traceSeq)
#define g_poolDeaths     (ii_default()->poolDeaths)
#define g_poolRecoveries (ii_default()->poolRecoveries)
#define g_cfiBase        (ii_default()->cfiBase)
#define g_cfiLen         (ii_default()->cfiLen)
#define g_cfiCount       (ii_default()->cfiCount)
#define g_bssScanned     (ii_default()->bssScanned)
// 薄壳池：必须落在目标 ±128MB 内，否则入口的 b 跳不过来。
// 一页 4KB 切 512 个 8 字节薄壳；多页按区域服务。
// 槽位大小：shell(28) + trampoline(最多 44)
//   之前是 64，trampoline 从 slot+32 开始只剩 32 字节，
//   而条件分支模式（b.cond/cbz/tbz）需要 44 字节 → 越界 12 字节，
//   会踩到相邻槽位的 shell 代码。
//   128 留足余量（当前最大需求 32+44=76）。
#define SHELL_SIZE      128
#define SHELL_PER_PAGE  512
#define II_MAX_PAGES    16

// .bss 池方案遗留：Android W^X 下对 rw-p 段 mprotect 返回 EACCES，
// 这条路走不通（见 INJECT_LOG 的 .bss 池实验），保留定义仅为对照，勿使用。
static void* g_shellPage[II_MAX_PAGES] __attribute__((unused)) = {0};
static int   g_shellUsed[II_MAX_PAGES] __attribute__((unused)) = {0};
static int   g_nShellPage __attribute__((unused)) = 0;

// 在 target 的 ±120MB 内找空隙分配一页（读 /proc/self/maps）
// ---------------------------------------------------------------------------
// .bss 零区池
//
// 背景：游戏 .so 是 275MB 连续 r-xp 段，±128MB 内没有可 mmap 的空隙
//       （-16MB 那片是 ART 的 PROT_NONE 预留，不能碰）。
//       但 .so 的 rw-p 段（.bss）里有大量从未使用的全零区域，
//       mprotect 加 X 后可以当 trampoline 池。
//
// 安全措施：
//   1. 只取 ≥64KB 的连续零区，且只用到边界前 4KB（留余量）
//   2. 池地址必须在目标 ±128MB 内（用相对 b 跳回的前提）
//   3. 首次使用时扫描一次，结果缓存
// ---------------------------------------------------------------------------
#define BSS_MIN_RUN   (64*1024)
#define BSS_POOL_MAX  4096


// 在 maps 中找属于 libminecraftpe 的 rw-p 段，扫描其零区，取离 target 最近的
static void* scan_bss_pool(uint64_t target) __attribute__((unused));
static void* scan_bss_pool(uint64_t target) {
    if (g_bssScanned) return MAP_FAILED;
    g_bssScanned = 1;

    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return MAP_FAILED;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long a, b;
        char perm[8] = {0};
        char path[256] = {0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]", &a, &b, perm, path) < 3) continue;
        if (strcmp(perm, "rw-p") != 0) continue;
        if (!strstr(path, "libminecraftpe")) continue;

        size_t len = b - a;
        if (len < BSS_MIN_RUN) continue;

        // 逐块读，找连续零区
        const size_t CHUNK = 65536;
        size_t bestStart = 0, bestLen = 0;
        size_t curStart = 0, curLen = 0;
        for (size_t off = 0; off < len; off += CHUNK) {
            size_t n = (off + CHUNK <= len) ? CHUNK : (len - off);
            const unsigned char* p = (const unsigned char*)(a + off);
            for (size_t i = 0; i < n; ++i) {
                if (p[i] == 0) {
                    if (curLen == 0) curStart = off + i;
                    ++curLen;
                } else {
                    if (curLen > bestLen) { bestLen = curLen; bestStart = curStart; }
                    curLen = 0;
                }
            }
            if (curLen > bestLen) { bestLen = curLen; bestStart = curStart; }
        }
        if (bestLen < BSS_MIN_RUN) continue;

        // 对齐到页，留 4KB 余量
        uint64_t poolAddr = (a + bestStart + 0xFFF) & ~0xFFFULL;
        size_t usable = bestLen - (poolAddr - (a + bestStart));
        if (usable < 4096) continue;
        usable = (usable > BSS_POOL_MAX) ? BSS_POOL_MAX : (usable & ~0xFFFULL);

        // 距离检查
        int64_t d = (int64_t)poolAddr - (int64_t)target;
        if (d <= -(1LL << 27) || d >= (1LL << 27)) continue;

        if (mprotect((void*)poolAddr, usable, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) continue;
        fclose(f);
        return (void*)poolAddr;
    }
    fclose(f);
    return MAP_FAILED;
}

static void* alloc_page_near(uint64_t target) __attribute__((unused));
static void* alloc_page_near(uint64_t target) {
    const long ps = sysconf(_SC_PAGESIZE);
    const uint64_t page = ps > 0 ? (uint64_t)ps : 4096;
    const uint64_t REACH = 0x7800000ULL;

    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return MAP_FAILED;
    static uint64_t lo[8192], hi[8192];
    int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) && n < 8192) {
        unsigned long a, b;
        if (sscanf(line, "%lx-%lx", &a, &b) == 2) { lo[n] = a; hi[n] = b; ++n; }
    }
    fclose(f);
    for (int i = 1; i < n; ++i) {
        uint64_t a = lo[i], b = hi[i]; int j = i - 1;
        while (j >= 0 && lo[j] > a) { lo[j+1]=lo[j]; hi[j+1]=hi[j]; --j; }
        lo[j+1]=a; hi[j+1]=b;
    }
    uint64_t best = 0, bestDist = ~0ULL;
    uint64_t winLo = target > REACH ? target - REACH : 0x10000ULL;
    uint64_t winHi = target + REACH;
    uint64_t cursor = winLo;
    for (int i = 0; i < n; ++i) {
        if (hi[i] <= winLo) continue;
        if (lo[i] >= winHi) break;
        if (lo[i] > cursor) {
            uint64_t gs = (cursor + page - 1) & ~(page - 1);
            if (gs + page <= lo[i]) {
                uint64_t d = gs > target ? gs - target : target - gs;
                if (d < bestDist) { bestDist = d; best = gs; }
            }
        }
        if (hi[i] > cursor) cursor = hi[i];
    }
    if (winHi > cursor) {
        uint64_t gs = (cursor + page - 1) & ~(page - 1);
        if (gs + page <= winHi) {
            uint64_t d = gs > target ? gs - target : target - gs;
            if (d < bestDist) { bestDist = d; best = gs; }
        }
    }
    if (!best) return MAP_FAILED;
    void* r = mmap((void*)best, page, PROT_READ|PROT_WRITE|PROT_EXEC,
                   MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
    if (r == MAP_FAILED || r != (void*)best) { if (r != MAP_FAILED) munmap(r, page); return MAP_FAILED; }
    return r;
}

// ---- 指令是否写 x16/x17（会用掉我们传递 trampoline 的寄存器）----
static int writes_ip0_ip1(uint32_t w) {
    // 粗判：目标寄存器字段在 bits[4:0] 的常见指令族
    uint32_t rd = w & 0x1F;
    // ldr/str 的 Rt、add/sub 的 Rd、mov 的 Rd、adrp 的 Rd 都在 bits[4:0]
    if (rd == 16 || rd == 17) return 1;
    // 成对访存 ldp/stp 的 Rt 也在 bits[4:0]（Rt2 在 bits[14:10]）
    if ((w & 0x3A000000) == 0x28000000) {
        uint32_t rt2 = (w >> 10) & 0x1F;
        if (rt2 == 16 || rt2 == 17) return 1;
    }
    return 0;
}

// 16 字节覆盖是否安全：覆盖 4 条指令，每条都不能是分支/ret/literal
static int uses_ip0_ip1(uint32_t insn);   // 定义在后面

static int can_cover_16(unsigned long addr) {
    // 16 字节入口覆盖 4 条指令。
    // 不仅要求这 4 条不是分支/PC 相对指令，还要求它们不碰 x16/x17 ——
    // 因为 trampoline 在远处，跳回只能用 ldr x16/br x16。
    for (int i = 0; i < 4; ++i) {
        uint32_t w = *(volatile uint32_t*)(addr + i*4);
        if (uses_ip0_ip1(w)) return 0;
        if (w == 0 || w == 0xFFFFFFFFu) return 0;
        if ((w & 0xFFFFFC1F) == 0xD65F0000) return 0;   // ret
        if ((w & 0xFC000000) == 0x14000000) return 0;   // b
        if ((w & 0xFF000010) == 0x54000000) return 0;   // b.cond
        if ((w & 0x7E000000) == 0x34000000) return 0;   // cbz
        if ((w & 0x7E000000) == 0x36000000) return 0;   // tbz
        if ((w & 0x3B000000) == 0x18000000) return 0;   // ldr-literal
        if ((w & 0x1F000000) == 0x10000000) return 0;   // adrp/adr
    }
    return 1;
}

// 指令是否读/写 x16 或 x17（含目标寄存器、源寄存器、ldp/stp 的 Rt2）
static int uses_ip0_ip1(uint32_t w) {
    if ((w & 0x1F) == 16 || (w & 0x1F) == 17) return 1;                       // Rt/Rd
    if ((w & 0x3A000000) == 0x28000000) {                                     // ldp/stp
        uint32_t rt2 = (w >> 10) & 0x1F;
        if (rt2 == 16 || rt2 == 17) return 1;
    }
    if (((w >> 5) & 0x1F) == 16 || ((w >> 5) & 0x1F) == 17) return 1;         // Rn
    return 0;
}

// 桩点之后 N 条指令是否都不碰 x16/x17（遇 ret/b 提前结束，视为安全）
// 用途：16 字节模式的跳回必须用 ldr x16，会破坏 x16。
//      只要后续代码不用它，破坏就无害。
static int next_n_clean(unsigned long addr, int n) {
    for (int k = 1; k <= n; ++k) {
        uint32_t w = *(volatile uint32_t*)(addr + k*4);
        if (w == 0 || w == 0xFFFFFFFFu) return 0;
        if (uses_ip0_ip1(w)) return 0;
        if ((w & 0xFFFFFC1F) == 0xD65F0000) break;   // ret
        if ((w & 0xFC000000) == 0x14000000) break;   // b
    }
    return 1;
}

int ii_can_instrument(unsigned long addr) {
    if (ii_find(addr)) return 0;                // 已装（真正的"不能重复装"）
    uint32_t w = *(volatile uint32_t*)addr;
    if (w == 0 || w == 0xFFFFFFFFu) return 0;   // 无效
    if (writes_ip0_ip1(w)) return 0;            // 会踩传递寄存器
    // 分支指令已支持（ret / b / b.cond / cbz / tbz）
    // ldr-literal 是「加载」不是跳转，搬移后语义会变，暂不支持
    if ((w & 0x3B000000) == 0x18000000) return 0;
    // 注意：next_n_clean 只在走 16 字节模式时才需要检查，
    // 放到 ii_install 的 16 字节分支里做（那时才确定用远模式）
    return 1;
}

// 取第 idx 个站点（越界或无块时返回 NULL）
static inline ii_site* site_at(int idx) {
    if (idx < 0 || idx >= g_nSites) return NULL;
    int c = idx / II_SITE_CHUNK;
    ii_site** blk = g_siteChunk[c];
    return blk ? blk[idx % II_SITE_CHUNK] : NULL;
}

// 设第 idx 个站点
static inline void site_set(int idx, ii_site* s) {
    if (idx < 0 || idx >= II_MAX_SITES) return;
    int c = idx / II_SITE_CHUNK;
    if (!g_siteChunk[c]) {
        g_siteChunk[c] = (ii_site**)calloc(II_SITE_CHUNK, sizeof(ii_site*));
        if (!g_siteChunk[c]) return;
        if (c + 1 > g_nChunk) g_nChunk = c + 1;
    }
    g_siteChunk[c][idx % II_SITE_CHUNK] = s;
}

// 追加一个站点，返回索引；失败返回 -1
static int site_append(ii_site* s) {
    if (!s) return -1;
    if (g_nSites >= II_MAX_SITES) return -1;
    int idx = g_nSites;

    // 直接检查块是否分配成功（不能调 site_at —— 它按 g_nSites 边界判断，
    // 而此刻 g_nSites 还没自增，必然返回 NULL）
    int c = idx / II_SITE_CHUNK;
    if (!g_siteChunk[c]) {
        g_siteChunk[c] = (ii_site**)calloc(II_SITE_CHUNK, sizeof(ii_site*));
        if (!g_siteChunk[c]) return -1;          // 内存不足
        if (c + 1 > g_nChunk) g_nChunk = c + 1;
    }
    g_siteChunk[c][idx % II_SITE_CHUNK] = s;

    g_nSites++;
    return idx;
}

int ii_count(void) { return g_nSites; }

ii_site* ii_find(unsigned long addr) {
    for (int i = 0; i < g_nSites; ++i) {
        ii_site* t = site_at(i);
        if (t && t->addr == addr) return t;
    }
    return NULL;
}

// 共享注册表（定义在文件后半部分）
static zh_region* g_reg  = NULL;
static int        g_myId = 0;

// 当前正在安装回调的实例 id（0 = 全局引擎）。
// ii_engine_install_ex 在调 ii_install_ex 前后设置/恢复它，
// chain_insert 据此给每个回调节点打上归属标记 —— 这样
// ii_engine_uninstall 才能精确摘掉"自己那份"，而不是连别人的一起摘。
// 用 __thread 是因为安装可能来自不同线程；单实例安装本身不带锁。
static __thread int g_curOwner = 0;

// 前向声明（定义在本文件后面）
static int  make_writable(unsigned long addr, size_t len);
static void make_readable(unsigned long addr, size_t len);
static void watchdog_start(void);

// 回调链管理（定义在 dispatch 之前）
static int  chain_insert(ii_site* s, ii_callback cb, void* user, int prio);
static int  chain_remove(ii_site* s, ii_callback cb, void* user);
static void chain_clear(ii_site* s);
static void wd_log(const char* fmt, ...);
static void* alloc_shell_near(uint64_t target, int* poolOut);
static void  pool_note_site(int poolIdx, unsigned long addr);
static uint32_t enc_movz_x17(uint32_t imm16);
static uint32_t enc_b(uint64_t from, uint64_t to);
static int  build_trampoline(void* tramp, uint32_t insn, unsigned long addr, int useRelJump, int isLight);

// 用离线表直接注册池（mn 毫秒级，不做全量扫描）
static uint64_t zh_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// ===========================================================================
// 崩溃现场记录（状态与热路径函数必须前置：ii_dispatch 比实现段更早）
// ===========================================================================

static inline void zh_trace_hit(uint64_t siteId, uint64_t pc) {
    if (!g_traceOn) return;
    uint32_t seq = __atomic_add_fetch(&g_traceSeq, 1, __ATOMIC_RELAXED);
    ii_trace_rec* r = &g_trace[(seq - 1) % II_TRACE_N];
    r->siteId = siteId;
    r->addr   = pc;
    r->t_ms   = zh_now_ms();
    r->seq    = seq;
}

// 游戏模块基址（缓存）。多条路径都要用：hint 池注册、rettype 查表。
static uint64_t zh_game_base(void) {
    static uint64_t cached = 0;
    if (cached) return cached;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    uint64_t base = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        unsigned long a, b; char perm[8]={0}; char path[512]={0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &a,&b,perm,path) < 3) continue;
        if (!strstr(path, "libminecraftpe")) continue;
        if (!base || a < base) base = a;
    }
    fclose(f);
    cached = base;
    return base;
}

int ii_register_hint_pools(const void* hints, int n) {
    if (!hints || n <= 0) return -1;
    if (!g_reg) { g_reg = zhreg_attach(); if (g_reg) g_myId = zhreg_owner(); }
    if (!g_reg) return -2;

    uint64_t base = zh_game_base();
    if (!base) return -3;

    struct hint { uint32_t vaddr; uint32_t size; };
    const struct hint* h = (const struct hint*)hints;
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        uint64_t addr = base + h[i].vaddr;
        uint64_t sz   = h[i].size;
        // 页对齐
        uint64_t al = (addr + 0xFFF) & ~0xFFFULL;
        sz = (sz - (al - addr)) & ~0xFFFULL;
        if (sz < 4096) continue;
        // hint 池：poolhint 表指向 libminecraftpe.so 内部的零填充空隙
        // （运行时实测这 5 处前 32 字节全是 0），适合放 shell 代码 ——
        // 它是唯一能落在 ±128MB 内的来源，4 字节 b 跳转必须靠它。
        //
        // 之前这里写坏游戏内存的原因不在 hint 本身，而在
        // zhreg_add_pool() 无条件执行
        //     *(uint32_t*)base = guard;
        // 往池首写 4 字节标记。hint 指向的不是我们的内存，写它就等于
        // 改游戏数据 —— 崩在别处、没有 tombstone，极难定位。
        //
        // 现在 zhreg_add_pool() 已按池来源区分：hint 池记 noGuard=1，
        // 不写任何字节，存活检测改用 mincore（只读，无副作用）。
        // 因此 hint 池可以安全启用了。
        if (mprotect((void*)al, sz, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) {
            LOG_W(LOG_POOL, "hint mprotect 失败 vaddr 0x%x errno=%d", h[i].vaddr, errno);
            continue;
        }
        int rc = zhreg_add_pool(g_reg, al, sz, "hint", g_myId);
        if (rc >= 0) { ++ok; LOG_I(LOG_POOL, "hint 池 + vaddr 0x%x %u KB", h[i].vaddr, h[i].size/1024); }
        else { LOG_W(LOG_POOL, "hint 注册失败 vaddr 0x%x rc=%d", h[i].vaddr, rc); }
    }
    LOG_I(LOG_POOL, "hint 池注册完成 %d/%d", ok, n);
    return ok;
}

int ii_init(void) {
    if (!g_reg) {
        g_reg = zhreg_attach();
        if (g_reg) g_myId = zhreg_owner();
    }
    if (g_reg) {
        // 每个探针都跑自己的 watchdog，但只迁移「自己 owner 的站点」。
        // 单 watchdog 不够：站点号是本探针 g_sites 的局部索引，跨探针无效，
        // 别人的 watchdog 没法替代你迁移你的站点。
        // 池死亡标记用 state 的幂等 CAS，谁先发现谁标记，大家各自迁移自己那份。
        wd_log("id=%d 启动 watchdog（各管各的站点）\n", g_myId);
        zhreg_wd_claim(g_reg, g_myId);   // 仅用于记录
        watchdog_start();
    } else {
        wd_log("zhreg_attach 失败，降级为单探针模式\n");
    }
    return 0;
}


// ===========================================================================
// 多池管理 —— trampoline 池的生存期管理
//
// 为什么不信任单个池：
//   .bss 零区 / 代码段填充区都是"当前没人用"，不是"永远没人用"。
//   游戏后续初始化全局变量、或者加载新代码段，都可能覆盖我们写进去的薄壳。
//   薄壳被覆盖 = 原地址的 b 跳进垃圾 = 崩。
//
// 方案：
//   1. 每个池开头 4 字节写 guard（magic 与池地址混合）
//   2. watchdog 线程每 500ms 校验所有活池的 guard
//   3. guard 变了 → 该池判定死亡 → 暂停全部线程 → 卸载该池所有站点 → 恢复线程
//   4. 重扫可用区域 → 新池 → 重装那些站点
// ===========================================================================
// ===========================================================================
// 池管理 —— 全部走 zhreg 共享区
//
// 多个探针 .so 同时加载时，静态变量各自独立，池表互相看不见。
// 共享区（mmap 固定文件）让所有探针：
//   - 看到同一份池表，可以互相分配槽位
//   - 只有一个 watchdog 在跑（CAS 抢占 + 心跳接管）
//   - 进程重启后自动重置（pid + starttime 校验）
// ===========================================================================
static volatile int  g_wdRun     = 0;
static pthread_t     g_wdTid;
static int           g_wdStarted = 0;

// watchdog 日志
static void wd_log(const char* fmt, ...) {
    int fd = open("/data/data/com.netease.x19/files/zhprobe/watchdog.log",
                  O_WRONLY|O_APPEND|O_CREAT, 0666);
    if (fd < 0) return;
    char buf[384];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    if (n > 0) (void)!write(fd, buf, n);
    close(fd);
}

// --- 暂停/恢复全部线程 ---
static volatile int g_stopReq = 0;
// ===== 暂停兜底机制 =====
// 教训：曾经把信号处理器里的 sched_yield() 换成 asm("yield") 纯自旋，
// 结果 235 个线程同时烧 CPU → 整机无响应，只能强制重启。
// 现在的原则：
//   1) 信号处理器里用 futex 等待（纯系统调用、async-signal-safe、不烧 CPU）
//   2) 看门狗线程豁免暂停，永远有线程能观察到卡死
//   3) 暂停超过 STOP_HARD_TIMEOUT_MS 强制恢复
//   4) 线程数超上限直接拒绝暂停（宁可装不上，不能拖死系统）
#define STOP_HARD_TIMEOUT_MS 2000
#define STOP_MAX_THREADS     600

static pid_t    g_wdLinuxTid = 0;             // 看门狗内核 TID（pthread_t 与 pid_t 不是一回事）
static volatile uint64_t g_pauseDeadline = 0; // 0 = 未处于暂停
static volatile long g_emergencyResumes = 0;   // long：与 (long*) 原子操作对齐

static void zh_stop_wake_all(void) {
    g_stopReq = 0;
    syscall(SYS_futex, (int*)&g_stopReq, FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL, NULL, 0);
}

static volatile int g_stopCount = 0;
static int          g_stopExpect = 0;

static void zh_stop_handler(int sig) {
    (void)sig;
    __sync_fetch_and_add((int*)&g_stopCount, 1);
    // 只能调 async-signal-safe 的函数。
    //   sched_yield()：POSIX 不保证（内核路径可能拿锁 → 与暂停互相死锁）
    //   asm("yield") ：纯自旋，多线程时把 CPU 烧光 → 整机卡死（已踩过）
    //   futex WAIT   ：纯系统调用，睡眠不占 CPU，被 resume 明确唤醒
    while (g_stopReq)
        syscall(SYS_futex, (int*)&g_stopReq, FUTEX_WAIT_PRIVATE, 1, NULL, NULL, 0);
}

// 暂停深度：site_enable/site_disable 内部也会 suspend，
// ii_apply_pending 在外面已经暂停过一层。不计数的话，
// 内层的 resume_others() 会把外层暂停的线程全部放掉，
// 外层继续在「线程正在跑」的状态下改写 16 字节。
static int g_pauseDepth = 0;

static int suspend_others(void) {
    if (g_pauseDepth > 0) {            // 已经处于暂停状态，直接成功
        ++g_pauseDepth;
        return g_stopCount;
    }
    static int inited = 0;
    if (!inited) {
        struct sigaction sa; memset(&sa, 0, sizeof(sa));
        sa.sa_handler = zh_stop_handler;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGRTMIN+3, &sa, NULL);
        inited = 1;
    }
    pid_t me   = getpid();
    pid_t self = (pid_t)syscall(SYS_gettid);
    DIR* d = opendir("/proc/self/task");
    if (!d) { g_pauseDepth = 0; g_stopReq = 0; return -1; }

    // 先数一遍：线程太多时这条路径本身就不安全，直接放弃
    // （每多一个线程就多一个「停在危险位置」的可能，且停不完的概率上升）
    int cap = 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9') ++cap;
    }
    rewinddir(d);
    if (cap > STOP_MAX_THREADS) {
        closedir(d);
        wd_log("暂停放弃：线程数 %d > 上限 %d\n", cap, STOP_MAX_THREADS);
        // 关键：这两个全局状态必须清干净。留了 g_stopReq=1 或
        // g_pauseDepth>0，后续每次 resume_others() 都只减计数、
        // 永远走不到 zh_stop_wake_all()，全进程线程会永久停在
        // do_signal_stop（真机上表现为整机卡死 + ANR）。
        g_pauseDepth = 0;
        g_stopReq    = 0;
        g_stopCount  = 0;
        g_stopExpect = 0;
        g_pauseDeadline = 0;
        return -1;
    }

    int n = 0;
    g_stopCount = 0;
    g_stopExpect = 0;
    g_pauseDeadline = zh_now_ms() + STOP_HARD_TIMEOUT_MS;   // 兜底死线
    g_stopReq = 1;
    g_pauseDepth = 1;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        pid_t t = (pid_t)atoi(e->d_name);
        if (t == self) continue;
        if (g_wdLinuxTid && t == g_wdLinuxTid) continue;   // 看门狗不暂停，否则没人能兜底
        syscall(SYS_tgkill, me, t, SIGRTMIN+3);
        ++n;
    }
    closedir(d);
    g_stopExpect = n;
    // 600ms：实测 319 线程时 300ms 常常停不全（游戏有 D 状态 IO 线程），
    // 而停不全会让 wideMode 改写被放弃。放宽到 600ms 显著提高成功率，
    // 代价只是 wideMode 这一罕见路径上多等一会儿（普通 4 字节路径根本不暂停）。
    for (int i = 0; i < 600; ++i) {
        if (g_stopCount >= g_stopExpect) break;
        usleep(1000);
    }
    if (g_stopCount >= g_stopExpect) g_pauseDeadline = 0;   // 停全了，解除死线
    return g_stopCount;
}

static void resume_others(void) {
    if (g_pauseDepth > 0) {            // 还有外层暂停
        --g_pauseDepth;
        if (g_pauseDepth > 0) return;
    }
    g_pauseDepth = 0;
    g_pauseDeadline = 0;
    zh_stop_wake_all();                // 清零 + futex 唤醒全部睡眠中的线程
}

// 卸载单个站点（不释放池内存）
static int site_restore(ii_site* s) __attribute__((unused));
static int site_restore(ii_site* s) {
    if (!s || !s->installed) return -1;
    unsigned long addr = s->addr;
    int len = s->wideMode ? 16 : 4;
    if (!make_writable(addr, len)) return -2;
    if (s->wideMode) {
        for (int i = 0; i < 4; ++i)
            *(uint32_t*)(addr + i*4) = *(volatile uint32_t*)(s->trampSaved + i*4);
    } else {
        *(uint32_t*)addr = s->orig;
    }
    __builtin___clear_cache((char*)addr, (char*)addr + len);
    make_readable(addr, len);
    s->installed = 0;
    return 0;
}

// --- 扫描：把可用区域注册进共享区 ---
static int __attribute__((unused)) scan_region_into_pools(uint64_t start, uint64_t len,
                                  const char* kind, size_t minRun) {
    if (!g_reg) return 0;
    int added = 0;
    const uint8_t* p = (const uint8_t*)start;
    size_t runStart = 0, runLen = 0;

    for (size_t off = 0; off + 4 <= len; off += 4) {
        uint32_t w; memcpy(&w, p + off, 4);
        int freeBlk = (w == 0 || w == 0xD503201Fu);
        if (freeBlk) {
            if (runLen == 0) runStart = off;
            runLen += 4;
        } else {
            if (runLen >= minRun) {
                uint64_t a  = start + runStart;
                uint64_t al = (a + 0xFFF) & ~0xFFFULL;
                size_t   sz = runLen - (size_t)(al - a);
                sz &= ~0xFFFULL;
                if (sz >= 1024) {
                    // 只有「我们自己的 mmap」才允许被写 guard；这里扫到的
                    // 都是进程已有映射（可能是游戏堆/栈/代码段），
                    // zhreg_add_pool 会据 kind 前缀判定 noGuard，绝不改写。
                    if (mprotect((void*)al, sz, PROT_READ|PROT_WRITE|PROT_EXEC) == 0) {
                        int rr = zhreg_add_pool(g_reg, al, sz, kind, g_myId);
                        if (rr >= 0) { ++added; wd_log("pool + %s 0x%llx %llu KB", kind, (unsigned long long)al, (unsigned long long)(sz/1024)); }
                        else wd_log("pool SKIP %s 0x%llx %llu KB rc=%d", kind, (unsigned long long)al, (unsigned long long)(sz/1024), rr);
                    } else {
                        wd_log("mprotect FAIL %s 0x%llx %llu KB", kind, (unsigned long long)al, (unsigned long long)(sz/1024));
                    }
                }
            }
            runLen = 0;
        }
    }
    if (runLen >= minRun) {
        uint64_t a  = start + runStart;
        uint64_t al = (a + 0xFFF) & ~0xFFFULL;
        size_t   sz = runLen - (size_t)(al - a);
        sz &= ~0xFFFULL;
        if (sz >= 1024) {
            if (mprotect((void*)al, sz, PROT_READ|PROT_WRITE|PROT_EXEC) == 0) {
                int rr = zhreg_add_pool(g_reg, al, sz, kind, g_myId);
                if (rr >= 0) { ++added; wd_log("pool + %s 0x%llx %llu KB", kind, (unsigned long long)al, (unsigned long long)(sz/1024)); }
                else wd_log("pool SKIP %s 0x%llx %llu KB rc=%d", kind, (unsigned long long)al, (unsigned long long)(sz/1024), rr);
            } else {
                wd_log("mprotect FAIL %s 0x%llx %llu KB", kind, (unsigned long long)al, (unsigned long long)(sz/1024));
            }
        }
    }
    return added;
}

int scan_pool_for_target(uint64_t target) {
    (void)target;
    if (!g_reg) return 0;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    int added = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        unsigned long a, b; char perm[8] = {0}; char path[512] = {0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &a, &b, perm, path) < 3) continue;
        if (!strstr(path, "libminecraftpe")) continue;
        size_t len = b - a;
        if (len < 4096) continue;

        // ★★ 绝不能把已有映射登记成 shell 池 ★★
        //
        // zhreg_add_pool() 会往 pool->base 的头 4 字节写 guard 标记。
        //   对 r-xp 段：写入触发 SIGSEGV（只读）。
        //   对 rw-p / rwxp 段：直接把游戏自己的数据/代码改掉。
        // 之前这里把 r-xp 和 rwxp 都登记进来，真机上表现为
        // 「重定向装好后几秒内卡死闪退，且没有 tombstone」——
        // 因为写坏的是游戏代码段/数据段，崩在别处。
        //
        // 现在只登记「确认是匿名且可写、且不属于任何文件映射」的空闲段，
        // 而且只用于 near 池候选，仍然要通过 zhreg_alloc_slot 的距离校验。
        if (strcmp(perm, "rw-p") != 0) continue;          // 只要可读写非执行
        if (path[0] != '\0') continue;                    // 只要匿名段
        added += scan_region_into_pools(a, len, "anon", 64*1024);
    }
    fclose(f);
    return added;
}

// 取一个槽位（从共享池里，任何探针注册的都能用）
// ---------------------------------------------------------------------------
// 在目标附近 mmap 一页，注册成 trampoline 池
//
// 为什么需要它：4 字节 b 只能跳 ±128MB。目标远离游戏 .so 时找不到近处空隙，
// 老做法是退化成 16 字节入口 —— 但那必须 suspend 全部线程，
// 而游戏有 D 状态 IO 线程，300ms 内停不全 → -17 放弃 → 卸不干净。
//
// 改成自己造池：用 MAP_FIXED_NOREPLACE 在目标 ±127MB 内按 64KB 步长找空位。
// 拿到近处池后就是普通的 4 字节原子写，既不需要 suspend，卸载也是原子写。
//
// 代价：最坏要试几千次 mmap。但只在「表里所有 hint 池都不覆盖该目标」时触发，
// 属于罕见路径（正常 hook 游戏函数永远命中 hint 池）。
// ---------------------------------------------------------------------------
#define NEAR_POOL_SIZE   (64u * 1024u)      // 64KB = 1024 个 slot
#define NEAR_POOL_STEP   (64u * 1024u)
// 试的次数要够：地址空间里 64KB 连续空洞很稀少，只试几十次几乎必然失败
// （实测只试 64 次时从 -127MB 起扫，区间全被占满 → 100% 失败）。
#define NEAR_POOL_MAX_TRIES 2048
#define NEAR_POOL_RANGE  (0x7F00000LL)      // ±127MB，留出安全边界

static int pool_from_mmap_near(uint64_t target) {
    if (!g_reg) return -1;
    // MAP_FIXED_NOREPLACE 是 Linux 4.17 引入的，Android 头文件里要 API 30+ 才声明。
    // 编译目标是 android26，所以这里手动定义（值固定为 0x100000）。
    // 运行时内核不支持的话 mmap 会返回 EINVAL，下面的 MAP_FAILED 分支会正常跳过。
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
    // ------------------------------------------------------------------
    // 策略：直接 mmap 一块 256MB 的窗口让它自己挑地址。
    // Linux 的内核在 mmap(NULL,...) 时，对 mmap_base 附近的大块请求
    // 会尽量贴近已有映射放下；命中 ±128MB 就用它，命中不了就换更小/更大。
    //
    // 为什么不用 MAP_FIXED_NOREPLACE 逐个试探：
    //   实测本游戏 ±128MB 范围内一个 64KB 空洞都没有（maps 里查过，
    //   1.7GB 的进程地址空间在 .so 附近已排满），逐点试探 100% 失败。
    // ------------------------------------------------------------------
    static const size_t TRY_SIZES[] = {
        NEAR_POOL_SIZE, 4*1024*1024, 16*1024*1024, 64*1024*1024
    };
    for (unsigned i = 0; i < sizeof TRY_SIZES/sizeof TRY_SIZES[0]; ++i) {
        size_t sz = TRY_SIZES[i];
        for (int t = 0; t < 64; ++t) {
            // 用 MAP_32BIT 等价手段：先试 MAP_FIXED_NOREPLACE 在 target 附近
            // 的系统推荐地址（mmap 返回值），再试普通 mmap。
            void* hint = (void*)((uintptr_t)(target + ((t & 1) ? (t+1)*NEAR_POOL_STEP
                                                             : -(t+1)*NEAR_POOL_STEP))
                                 & ~0xFFFULL);
            void* p = mmap(hint, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);   // 有 hint，内核会尽量靠拢
            if (p == MAP_FAILED) {
                p = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (p == MAP_FAILED) break;
            }
            int64_t d = (int64_t)(uintptr_t)p - (int64_t)target;
            if (d > -(1LL << 27) && d < (1LL << 27)) {
                // 只要前 64KB 当池，其余立刻还给系统
                if (sz > NEAR_POOL_SIZE) munmap((void*)((uintptr_t)p + NEAR_POOL_SIZE),
                                                sz - NEAR_POOL_SIZE);
                // kind="own"：明确告诉 zhreg 这是我们自己 mmap 的池，
                                        // 允许写 guard 标记（独占内存，写它安全）
                                        int rc = zhreg_add_pool(g_reg, (uint64_t)(uintptr_t)p, NEAR_POOL_SIZE,
                                                                "own", g_myId);
                if (rc >= 0) {
                    wd_log("near 池 @0x%llx 覆盖 target=0x%llx（size=%zu, 第 %d 次）\n",
                           (unsigned long long)(uintptr_t)p,
                           (unsigned long long)target, sz, t);
                    return 1;
                }
                munmap(p, NEAR_POOL_SIZE);
                return -1;
            }
            munmap(p, sz);
        }
    }
    wd_log("near 池失败 target=0x%llx（连 256MB 窗口都放不进 ±128MB）\n",
           (unsigned long long)target);
    return -1;
}

static void* alloc_shell_near(uint64_t target, int* poolOut) {
    if (poolOut) *poolOut = -1;
    if (!g_reg) { wd_log("[alloc_shell] g_reg 为空\n"); return NULL; }

    int pi = -1;
    void* slot = zhreg_alloc_slot(g_reg, target, SHELL_SIZE, &pi, g_myId);
    wd_log("[alloc_shell] target=0x%llx 首轮 slot=%p pool=%d (nPool=%d)\n",
           (unsigned long long)target, slot, pi, g_reg->nPool);
    if (!slot) {
        // 共享区里没有合适池 → 扫一次进程映射把候选登记进共享区。
        //
        // 安全性由 zhreg_add_pool 保证：这些候选段不是我们 mmap 的，
        // 登记时 noGuard=1，绝不往段头写 guard 标记（写 r-xp 会 SIGSEGV，
        // 写 rw-p 会改游戏数据）。存活检测用 mincore，只读无副作用。
        int n2 = scan_pool_for_target(target);
        if (n2 > 0) slot = zhreg_alloc_slot(g_reg, target, SHELL_SIZE, &pi, g_myId);
    }
    if (!slot) {
        // 还是没有 → 在目标附近自己 mmap 造一个池。
        // 这样就能走 4 字节原子写，不需要 suspend，卸载也不会因停不全而失败。
        int pr = pool_from_mmap_near(target);
        wd_log("[alloc_shell] pool_from_mmap_near 返回 %d\n", pr);
        if (pr > 0)
            slot = zhreg_alloc_slot(g_reg, target, SHELL_SIZE, &pi, g_myId);
    }
    wd_log("[alloc_shell] 最终 slot=%p pool=%d\n", slot, pi);
    if (slot && poolOut) *poolOut = pi;
    return slot;
}

static void pool_note_site(int poolIdx, unsigned long addr) {
    if (g_reg) zhreg_note_site(g_reg, poolIdx, addr);
}

// --- 池死亡后的恢复 ---
static int recover_pool(int idx) {
    if (!g_reg || idx < 0 || idx >= g_reg->nPool) return -1;
    zh_pool_rec* p = &g_reg->pool[idx];
    if (p->state == 1) {
        // 幂等标记：只有一个探针能把它从 1 翻到 2 并计数死亡
        int32_t prev = __sync_val_compare_and_swap(&p->state, 1, 2);
        if (prev == 1) __sync_fetch_and_add((long*)&g_poolDeaths, 1);
    }
    if (p->state != 2) return -1;

    // 收集本探针在该池里的站点（ii_find 只查本地表 → 天然只拿到自己的）
    unsigned long addrs[ZHREG_SITES_PER_POOL];
    ii_site*      sites[ZHREG_SITES_PER_POOL];
    int           sids [ZHREG_SITES_PER_POOL];
    int n = 0;
    for (int i = 0; i < p->nSite && i < ZHREG_SITES_PER_POOL; ++i) {
        ii_site* s = ii_find(p->siteAddr[i]);
        if (!s || !s->installed) continue;
        int sid = -1;
        for (int k = 0; k < g_nSites; ++k) if (site_at(k) == s) { sid = k; break; }
        if (sid < 0) continue;
        addrs[n] = s->addr; sites[n] = s; sids[n] = sid; ++n;
    }
    if (n == 0) return 0;

    // 不暂停线程：每个站点的迁移只有最后一笔「改 4 字节 b」是关键操作，
    // 而 ARM64 对齐 32 位写天然原子 —— 跑到这里的线程要么已取指（继续走旧壳），
    // 要么还没到（下次取到新 b）。新壳在改 b 之前就写好了，不存在裸奔窗口。
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        unsigned long a = addrs[i];
        ii_site* s = sites[i];

        int pi = -1;
        void* slot = alloc_shell_near(a, &pi);
        if (!slot) continue;

        uint32_t* sh = (uint32_t*)slot;
        if (s->rawDetour) {
            // 直通模式：shell 是 16 字节绝对跳转，不压栈。
            // 若这里按指令级的 24 字节布局写，迁移后会跳到 ii_common，
            // 而 RAW 站点没有回调、栈布局也对不上 → 必崩。
            sh[0] = 0x58000050u;                    // ldr x16, #8
            sh[1] = 0xD61F0200u;                    // br  x16
            *(unsigned long*)(sh + 2) = (unsigned long)s->rawDetour;
        } else {
            sh[0] = 0xA9BF47F0u;                    // stp x16, x17, [sp, #-16]!
            sh[1] = enc_movz_x17((uint32_t)sids[i]); // mov x17, #sid
            sh[2] = 0x58000050u;                    // ldr x16, #8
            sh[3] = 0xD61F0200u;                    // br  x16
            *(unsigned long*)(sh + 4) = s->lightCb ? (unsigned long)ii_light : (unsigned long)ii_common;
        }

        void* tramp = (void*)((char*)slot + 32);
        // RAW 与 LIGHT 一样不需要 trampoline 前导（shell 都没动 sp）
        int tlen = build_trampoline(tramp, s->orig, a, 1,
                                    s->rawDetour != NULL || s->lightCb != NULL);
        if (tlen <= 0) continue;
        __builtin___clear_cache((char*)slot, (char*)slot + 32 + tlen);

        uint32_t b = enc_b(a, (uint64_t)slot);
        if (!b) continue;
        if (!make_writable(a, 4)) continue;
        *(uint32_t*)a = b;
        __builtin___clear_cache((char*)a, (char*)a + 4);
        make_readable(a, 4);

        s->shell      = (unsigned long)slot;
        s->trampoline = (unsigned long)tramp;
        s->poolIdx    = pi;
        if (pi >= 0) pool_note_site(pi, a);

        // 从旧池的站点表里摘掉，防止下个 tick 重复迁移
        for (int k = 0; k < p->nSite; ++k)
            if (p->siteAddr[k] == a) { p->siteAddr[k] = 0; break; }
        ++ok;
    }

    // 计数器：之前只在 g_poolDeaths 上加了，recover 侧漏了，
    // 导致日志里能看到 deaths=1 但 recov=0。
    if (ok > 0) __sync_fetch_and_add((long*)&g_poolRecoveries, 1);

    return ok;
}

// --- watchdog：全进程只该有一个 ---
static void* watchdog_main(void* arg) {
    (void)arg;
    // 记下自己，suspend_others 会跳过本线程 —— 否则暂停时连兜底的人都没了
    g_wdLinuxTid = (pid_t)syscall(SYS_gettid);
    wd_log("watchdog start id=%d tid=%d pools=%d\n",
           g_myId, (int)g_wdLinuxTid, g_reg ? g_reg->nPool : -1);
    long ticks = 0;
    while (g_wdRun) {
        ++ticks;

        // ===== 卡死兜底 =====
        // 主线程若在 suspend 之后卡住（mprotect 阻塞、被别的信号打断、
        // 中途 return 漏调 resume），其余线程会一直在 futex 上睡眠，
        // 整个进程失去响应。这里强制兜住：超过死线就唤醒全部线程。
        if (g_pauseDeadline && zh_now_ms() > g_pauseDeadline) {
            __sync_fetch_and_add(&g_emergencyResumes, 1);
            wd_log("!!! 紧急恢复：暂停超时 %llu ms，强制唤醒全部线程（累计 %d 次）\n",
                   (unsigned long long)(zh_now_ms() - (g_pauseDeadline - STOP_HARD_TIMEOUT_MS)),
                   g_emergencyResumes);
            g_pauseDepth = 0;
            g_pauseDeadline = 0;
            g_stopExpect = 0;
            zh_stop_wake_all();
        }

        // 心跳：告诉别的探针我还活着
        if (g_reg) zhreg_wd_beat(g_reg);

        // 注意：不要因为 wdOwner 不是自己就退出。
        // 「各管各的站点」模型下每个探针都要有活着的 watchdog，
        // 否则自己的站点在池死亡后无人迁移 → 崩。
        // wdOwner 只作为「最近活跃者」的观测字段。

        for (int i = 0; g_reg && i < g_reg->nPool; ++i) {
            zh_pool_rec* q = &g_reg->pool[i];
            if (q->state == 0) continue;

            int dead = (q->state == 2) || !zhreg_pool_alive(g_reg, i);
            if (!dead) continue;

            // 池死了：只处理自己还有站点留在这里的情况
            int mine = 0;
            for (int k = 0; k < q->nSite; ++k)
                if (q->siteAddr[k] && ii_find(q->siteAddr[k])) { mine = 1; break; }
            if (!mine) continue;

            wd_log("POOL DEAD #%d base=0x%llx expect=0x%x sites=%d id=%d\n",
                   i, (unsigned long long)q->base, q->guard, q->nSite, g_myId);
            int r = recover_pool(i);
            wd_log("recover(#%d) -> %d id=%d\n", i, r, g_myId);
        }

        if ((ticks % 20) == 0)
            wd_log("heartbeat id=%d ticks=%ld pools=%d deaths=%ld recov=%ld\n",
                   g_myId, ticks, g_reg ? g_reg->nPool : -1, g_poolDeaths, g_poolRecoveries);

        ii_apply_pending();     // 有待办才动线程，否则零开销

        // 200ms 一拍：兜底死线是 2s，要留足余量才能及时介入
        for (int k = 0; k < 2 && g_wdRun; ++k) usleep(100000);
    }
    return NULL;
}

static void watchdog_start(void) {
    if (g_wdStarted) return;
    // 诊断开关：环境变量 ZH_NO_WATCHDOG=1 时不启动后台线程。
    // 用于隔离「引擎后台线程」与「hook 路径」两类问题。
    // 生产环境不要设置该变量（watchdog 负责池死亡恢复和暂停兜底）。
    {
        const char* e = getenv("ZH_NO_WATCHDOG");
        if (e && e[0] == '1') {
            wd_log("ZH_NO_WATCHDOG=1，跳过 watchdog 启动\n");
            return;
        }
    }
    g_wdStarted = 1;
    g_wdRun = 1;
    int rc = pthread_create(&g_wdTid, NULL, watchdog_main, NULL);
    if (rc == 0) {
        // 等看门狗登记自己的内核 TID 再返回。
        // 否则这之前发生的 suspend_others 会把看门狗一并暂停，
        // 死线到了也没人执行强制恢复 → 整机卡死。
        for (int i = 0; i < 300 && !g_wdLinuxTid; ++i) usleep(1000);
        if (!g_wdLinuxTid) wd_log("警告：看门狗未在 300ms 内登记 TID\n");
    }
    if (rc != 0) {
        g_wdStarted = 0; g_wdRun = 0;
        wd_log("watchdog_start FAILED rc=%d\n", rc);
    }
}

// 池状态导出
int ii_pool_stats(char* buf, int len) {
    int off = 0;
    if (!g_reg) {
        off += snprintf(buf+off, len-off, "zhreg not attached\n");
        return off;
    }
    off += snprintf(buf+off, len-off, "myId=%d pools=%d deaths=%ld recov=%ld wdOwner=%u\n",
                    g_myId, g_reg->nPool, g_poolDeaths, g_poolRecoveries, g_reg->wdOwner);
    for (int i = 0; i < g_reg->nPool; ++i) {
        zh_pool_rec* q = &g_reg->pool[i];
        if (q->state != 1) { off += snprintf(buf+off, len-off, "  #%d DEAD\n", i); continue; }
        off += snprintf(buf+off, len-off, "  #%d %-6s owner=%d vaddr 0x%llx %llu/%llu sites=%d\n",
                        i, q->kind, q->owner,
                        (unsigned long long)q->base,
                        (unsigned long long)q->used,
                        (unsigned long long)q->size, q->nSite);
    }
    return off;
}

// ctx 里的取消标志位（pstate bit63）。
// nzcv 只用低 4 位，高位是 RES0，handler 恢复时的 msr nzcv 会忽略它。
#define II_CANCEL_BIT      (1ULL << 63)
#define II_IS_CANCELLED(c) (((c)->pstate & II_CANCEL_BIT) != 0)

// 同 so 内的兜底标志（c 为 NULL 时用）
// ★ 不能是 __thread：musl 会为它生成 R_AARCH64_TLSDESC 重定位，
//   Android bionic 不认识这种重定位，整个 .so 会被拒绝加载（实测）。
//   本变量只在「同 .so 内 fallback」用，跨 .so 一律走 ctx->pstate 的 cancel 位。
static int g_cancelReq = 0;

// 取消请求
//
// 必须通过 ctx 传递，不能用 __thread 变量：每个探针 .so 各有一份 g_cancelReq，
// 探针 B 的回调里调 ii_cancel() 只会置位 B 自己那份，A 的 handler 读不到。
// 走 ctx 才能跨 .so 生效。
void ii_cancel(ii_ctx* c) {
    if (c) c->pstate |= II_CANCEL_BIT;
    else   g_cancelReq = 1;
}
// ===========================================================================
// 回调链（同一地址多回调，对标 Mixin 的多个 @Inject）
// ===========================================================================

static ii_cb_node* cbnode_new(ii_callback cb, void* user, int prio) {
    ii_cb_node* n = (ii_cb_node*)malloc(sizeof *n);
    if (!n) return NULL;
    n->next = NULL; n->cb = cb; n->user = user;
    n->priority = prio; n->alive = 1;
    return n;
}

static void cbnode_free_chain(ii_cb_node* head) {
    while (head) { ii_cb_node* nx = head->next; free(head); head = nx; }
}

static int chain_insert(ii_site* s, ii_callback cb, void* user, int prio) {
    if (!s || !cb) return -1;
    ii_cb_node* n = cbnode_new(cb, user, prio);
    if (!n) return -9;
    n->owner = g_curOwner;      // 打归属标记（见 g_curOwner 说明）
    if (!s->cbs || s->cbs->priority < prio) {
        n->next = s->cbs; s->cbs = n;
    } else {
        ii_cb_node* p = s->cbs;
        while (p->next && p->next->priority >= prio) p = p->next;
        n->next = p->next; p->next = n;
    }
    s->nCbs++;
    return 0;
}

static int chain_remove(ii_site* s, ii_callback cb, void* user) {
    if (!s) return -1;
    ii_cb_node** pp = &s->cbs;
    while (*pp) {
        ii_cb_node* n = *pp;
        if (n->cb == cb && n->user == user) {
            // ★ 懒删除：只把 alive 清 0，不从链上摘、也不 free。
            //
            // 原因：ii_dispatch() 在热路径上遍历这条链（每次函数被调用都会走）。
            // 若这里直接 *pp = n->next; free(n)，正在遍历的线程下一轮读
            // n->next 就是 use-after-free —— 真机表现是"卸载后立刻卡死闪退"，
            // 因为被 hook 的是每帧调用的函数，几乎必然撞上。
            //
            // ii_dispatch 里本来就有 `if (!n->alive || !n->cb) continue;`
            // 跳过死节点，所以保留节点无副作用；真正的 free 留到
            // chain_clear()（站点整体销毁时，那时入口已恢复原指令）。
            n->alive = 0;
            if (s->nCbs > 0) s->nCbs--;
            return 0;
        }
        pp = &n->next;
    }
    return -1;
}

static void chain_clear(ii_site* s) {
    if (!s) return;
    cbnode_free_chain(s->cbs);
    s->cbs = NULL; s->nCbs = 0;
}

int ii_add_callback(unsigned long addr, ii_callback cb, void* user, int priority) {
    if (!addr || !cb) return -1;
    ii_site* s = ii_find(addr);
    if (!s) { wd_log("ii_add_callback: 站点 0x%lx 不存在，先调 ii_install\n", addr); return -2; }
    if (s->lightCb) return -16;
    return chain_insert(s, cb, user, priority);
}

int ii_remove_callback(unsigned long addr, ii_callback cb, void* user) {
    ii_site* s = ii_find(addr);
    return s ? chain_remove(s, cb, user) : -1;
}

int ii_callback_count(unsigned long addr) {
    ii_site* s = ii_find(addr);
    return s ? s->nCbs : 0;
}

// ---- 归属（模块分组）----
//
// owner 存的是指针，调用者必须保证字符串生命周期（通常用字符串字面量）。

void ii_set_owner(unsigned long addr, const char* owner) {
    ii_site* s = ii_find(addr);
    if (s) s->owner = owner;
}

const char* ii_get_owner(unsigned long addr) {
    ii_site* s = ii_find(addr);
    return s ? s->owner : NULL;
}

int ii_uninstall_owner(const char* owner) {
    if (!owner) return -1;
    int n = 0;
    // 倒序遍历：卸载会清空槽位，正序会跳过元素
    for (int i = g_nSites - 1; i >= 0; --i) {
        ii_site* s = site_at(i);
        if (!s) continue;
        if (!s->owner || __builtin_strcmp(s->owner, owner) != 0) continue;
        if (ii_uninstall(s->addr) == 0) ++n;
    }
    if (n) wd_log("uninstall_owner(\"%s\"): 卸载 %d 个站点\n", owner, n);
    return n;
}



// 调度：由 handler 调用。c 指向栈上的 ii_ctx。
// 返回要跳转的地址（trampoline 或 cancel 跳板）。
void* ii_dispatch(ii_ctx* c, uint64_t slotId) {
    ii_site* s = site_at((int)slotId);
    if (!s) {
        // ★ 绝不能返回 NULL：ii_common 尾部是 ldr x16,[sp,#0x100]; br x16，
        //   拿到 NULL 就直接跳到地址 0，整进程立刻闪退且没有 tombstone
        //   （t11 真机现象：destroy 掉站点后几秒内进程消失）。
        //
        //   查不到站点说明 sid 与全局表不匹配（站点已被卸载）。
        //   此时唯一安全的去处是"当作函数已正常返回"：
        //   c->pc 是调用点地址，x30 设成它下游即可。
        //   这里返回 trampoline 上一个无效地址不可行，直接返回 0，
        //   由汇编端的 cbz 兜底走 ret。
        return NULL;
    }
    c->pc = s->addr;

    g_cancelReq = 0;
    if (g_traceOn) zh_trace_hit(slotId, s->addr);

    // 本地回调：优先走回调链（支持同一地址多个 @Inject）
    // 单回调时链表只有一个节点，无额外开销。
    // 兼容：老路径（重定向/RAW）用 s->cb，链为空时才走它。
    if (s->cbs) {
        for (ii_cb_node* n = s->cbs; n; n = n->next) {
            if (!n->alive || !n->cb) continue;
            // 耗时保护：回调跑在游戏线程上，一旦阻塞就会冻结整个游戏
            // （ANR 阈值 5 秒，实测 sleep(2)×15 必崩）。
            // 这里只在超过阈值时告警一次，不阻止 —— 用户可能确实需要。
            uint64_t t0 = zh_now_ms();
            n->cb(c, n->user);
            uint64_t dt = zh_now_ms() - t0;
            if (dt >= II_CB_SLOW_MS) {
                int seen = 0;
                for (ii_cb_node* q = s->cbs; q; q = q->next)
                    if (q->slowWarned) { seen = 1; break; }
                if (!seen) {
                    n->slowWarned = 1;
                    wd_log("[SLOW] 回调 @0x%lx 耗时 %llu ms（阈值 %d）。"
                           "回调运行在游戏线程上，重操作会冻结游戏，请改为异步。\n",
                           s->addr, (unsigned long long)dt, II_CB_SLOW_MS);
                }
            }
            if (g_cancelReq || II_IS_CANCELLED(c)) break;
        }
    } else if (s->cb) {
        uint64_t t0 = zh_now_ms();
        s->cb(c, s->user);
        uint64_t dt = zh_now_ms() - t0;
        if (dt >= II_CB_SLOW_MS && !s->cbSlowWarned) {
            s->cbSlowWarned = 1;
            wd_log("[SLOW] 回调 @0x%lx 耗时 %llu ms（阈值 %d）。"
                   "回调运行在游戏线程上，重操作会冻结游戏，请改为异步。\n",
                   s->addr, (unsigned long long)dt, II_CB_SLOW_MS);
        }
    }

    // 其它探针挂在这个站点上的回调。
    // 单探针时 foreign==0，连共享区都不碰，热路径零额外开销。
    if (s->regIdx >= 0 && g_reg && !g_cancelReq && !II_IS_CANCELLED(c)) {
        if (zhreg_foreign_cb(g_reg, s->regIdx, g_myId) > 0) {
            zh_site_rec* zs = &g_reg->site[s->regIdx];
            for (int k = 0; k < zs->nCb && k < ZHREG_MAX_CB; ++k) {
                if (!zs->cb[k].alive || zs->cb[k].owner == g_myId) continue;
                ((ii_callback)(uintptr_t)zs->cb[k].cb)(c, (void*)(uintptr_t)zs->cb[k].user);
                if (g_cancelReq || II_IS_CANCELLED(c)) break;
            }
        }
    }

    if (g_cancelReq || II_IS_CANCELLED(c)) {
        g_cancelReq = 0;
        c->pstate &= ~II_CANCEL_BIT;      // 清标志，恢复 pstate 时不该带出去
        // 类型校验：VOID/STRUCT 返回的函数被 cancel 时，x0 的值没有意义，
        // 但更危险的是「往 float 返回的函数里塞整数」—— 写错寄存器，静默出错。
        // 只告警一次，不阻止（调用方可能确实知道自己在做什么）。
        if (s->retKind == II_RET_VOID || s->retKind == II_RET_STRUCT) {
            static int w1 = 0;
            if (!w1) { w1 = 1;
                wd_log("cancel 告警 @0x%lx：该函数返回 %s，设返回值无意义\n",
                       s->addr, ii_ret_type_name(s->addr)); }
        }
        if (!s->cancelable) {
            // 非入口站点 cancel 会破坏栈帧，拒绝并继续原指令
            static int warned = 0;
            if (!warned) { warned = 1; wd_log("cancel 被拒绝：非入口站点 @0x%lx\n", s->addr); }
            return (void*)s->trampoline;
        }
        return (void*)ii_cancel_tramp;
    }
    return (void*)s->trampoline;
}

// 轻量调度（高频路径）—— 和 ii_dispatch 同构，但用 ii_ctx_light
// 注：LIGHT 不支持 cancel —— ii_ctx_light 里没有 pstate，也没有返回地址，
// 跳过函数体无法安全返回。
// ===========================================================================
// 崩溃现场记录
// ===========================================================================
void ii_trace_begin(void) { g_traceOn = 1; }
void ii_trace_end(void)   { g_traceOn = 0; }
int  ii_trace_is_on(void) { return g_traceOn; }

void ii_trace_dump(void) {
    uint32_t seq = __atomic_load_n(&g_traceSeq, __ATOMIC_ACQUIRE);
    int n = (seq < II_TRACE_N) ? (int)seq : II_TRACE_N;
    wd_log("=== trace dump: 共 %u 次命中，显示最近 %d 条 ===", seq, n);
    for (int i = 1; i <= n; ++i) {
        uint32_t want = seq - i;
        ii_trace_rec* r = &g_trace[want % II_TRACE_N];
        if (__atomic_load_n(&r->seq, __ATOMIC_ACQUIRE) != seq - i + 1) continue;
        wd_log("  [%u] site=%llu pc=0x%llx t=%llums",
               r->seq, (unsigned long long)r->siteId,
               (unsigned long long)r->addr, (unsigned long long)r->t_ms);
    }
}

int ii_trace_read(int idx, ii_trace_rec* out) {
    if (!out || idx < 0 || idx >= II_TRACE_N) return -1;
    uint32_t seq = __atomic_load_n(&g_traceSeq, __ATOMIC_ACQUIRE);
    if ((uint32_t)idx >= seq) return -2;
    uint32_t want = seq - 1 - (uint32_t)idx;          // 0 = 最近
    *out = g_trace[want % II_TRACE_N];
    return 0;
}

// 崩溃时转储（信号处理器里只能调 async-signal-safe：用 write 直接写 fd）
// 崩溃转储 —— 只能用 async-signal-safe 调用
//
// 原实现用了 snprintf / signal() / raise()，三者都不在 POSIX 的
// async-signal-safe 列表里。snprintf 可能 malloc 或加 stdio 锁 ——
// 崩溃瞬间若另一线程正持锁，处理器会死锁，进程卡死且 tombstone 不生成，
// 用户只看到"闪退但没有任何日志"。
//
// 现改为：自写整数转换 + write + sigaction + kill，全部安全。
static void zh_crash_handler(int sig) {
    char buf[256];
    int n = 0;

    n = zh_sh_str("\n!!! SIG ", buf, n);
    n += zh_sh_dec((uint32_t)sig, buf + n);
    n = zh_sh_str(" !!! 最后命中:\n", buf, n);
    zh_sh_emit(buf, n);

    uint32_t seq = __atomic_load_n(&g_traceSeq, __ATOMIC_ACQUIRE);
    uint32_t cnt = (seq < 8) ? seq : 8;
    for (uint32_t i = 1; i <= cnt; ++i) {
        uint32_t want = seq - i;
        ii_trace_rec r = g_trace[want % II_TRACE_N];

        n = zh_sh_str("  [", buf, 0);
        n += zh_sh_dec(r.seq, buf + n);
        n = zh_sh_str("] site=", buf, n);
        n += zh_sh_dec((uint32_t)r.siteId, buf + n);
        n = zh_sh_str(" pc=0x", buf, n);
        n += zh_sh_hex(r.addr, buf + n);
        n = zh_sh_str("\n", buf, n);
        zh_sh_emit(buf, n);
    }

    // 恢复默认处理（sigaction 是 async-signal-safe；signal() 不是）
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sa.sa_flags   = SA_RESETHAND;
    // 不用 sigemptyset（实现可能不走 syscall），直接清零
    for (size_t k = 0; k < sizeof(sa.sa_mask); ++k)
        ((unsigned char*)&sa.sa_mask)[k] = 0;
    sa.sa_restorer = NULL;
    sigaction(sig, &sa, NULL);

    // 重新触发以生成 tombstone（kill 安全；raise 走 pthread_kill 不安全）
    kill(getpid(), sig);
}

int ii_trace_install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = zh_crash_handler;
    sa.sa_flags = SA_RESETHAND;      // 触发一次后恢复默认
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) != 0) return -1;
    if (sigaction(SIGABRT, &sa, NULL) != 0) return -1;
    return 0;
}

// 调用重定向的调度：返回目标地址（NULL = 用原目标）
//
// 同时把 callSite+4 写进 ctx 的 pc 槽，handler 会读它设 x30 ——
// 这样目标函数 ret 时正好回到原 bl 的下一条指令。
void* ii_redirect_dispatch(ii_ctx* c, uint64_t slotId) {
    ii_site* s = site_at((int)slotId);
    {
        static int dbg = 0;
        if (__atomic_exchange_n(&dbg, 1, __ATOMIC_RELAXED) == 0)
            wd_log("[redir_dispatch] 首次 slotId=%llu nSites=%d site=%p\n",
                   (unsigned long long)slotId, g_nSites, (void*)s);
    }
    if (!s) {
        // ★ 绝不能返回 NULL：汇编端拿到 NULL 会直接 br x16 跳到地址 0，
        //   整进程立刻闪退（t12 真机现象：装好后 4~6 秒内退出，无 tombstone）。
        //   查不到站点说明 slotId 与当前实例表不匹配，此时没法知道原目标，
        //   只能跳到"返回地址"让调用方当作调用已返回 —— 至少不会野跳。
        if (c) return (void*)(uintptr_t)c->pc;
        return (void*)(uintptr_t)0;
    }
    if (g_traceOn) zh_trace_hit(slotId, s->callSite);

    // ★ c->pc = 目标函数返回后该回到哪里（汇编端装进 x30）。
    //   4 字节模式：bl 只换成 1 条 b，返回点就是 callSite+4。
    //   16 字节模式：callSite..callSite+16 全被覆盖，原 bl 和后面 3 条
    //                指令都搬进了 trampoline，返回点必须是 trampoline
    //                （它执行那 3 条后再跳 callSite+16）。
    //   若这里仍用 callSite+4，目标 ret 会回到已经被写成 br x16 的
    //   那条指令上 —— 死循环或野跳。
    c->pc = (s->wideMode && s->trampoline)
              ? ((uint64_t)s->trampoline + 4096u)
              : (uint64_t)(s->callSite + 4);

    void* tgt = NULL;
    if (s->redirCb) tgt = ((ii_redirect_cb)s->redirCb)(c, s->user);
    return tgt ? tgt : (void*)s->origTarget;
}

void* ii_dispatch_light(ii_ctx_light* c, uint64_t slotId) {
    ii_site* s = site_at((int)slotId);
    if (!s) return NULL;
    if (g_traceOn) zh_trace_hit(slotId, s->addr);
    if (s->lightCb) s->lightCb(c, s->user);

    if (s->regIdx >= 0 && g_reg) {
        if (zhreg_foreign_cb(g_reg, s->regIdx, g_myId) > 0) {
            zh_site_rec* zs = &g_reg->site[s->regIdx];
            for (int k = 0; k < zs->nCb && k < ZHREG_MAX_CB; ++k) {
                if (!zs->cb[k].alive || zs->cb[k].owner == g_myId) continue;
                ((ii_light_callback)(uintptr_t)zs->cb[k].cb)(c, (void*)(uintptr_t)zs->cb[k].user);
            }
        }
    }
    return (void*)s->trampoline;
}

// ---------------------------------------------------------------------------
// 代码页可写化
// ---------------------------------------------------------------------------
static int make_writable(unsigned long addr, size_t len) {
    long ps = sysconf(_SC_PAGESIZE);
    unsigned long page = (unsigned long)(ps > 0 ? ps : 4096);
    unsigned long start = addr & ~(page - 1);
    unsigned long end = (addr + len + page - 1) & ~(page - 1);
    return mprotect((void*)start, end - start, PROT_READ|PROT_WRITE|PROT_EXEC) == 0;
}
static void make_readable(unsigned long addr, size_t len) {
    long ps = sysconf(_SC_PAGESIZE);
    unsigned long page = (unsigned long)(ps > 0 ? ps : 4096);
    unsigned long start = addr & ~(page - 1);
    unsigned long end = (addr + len + page - 1) & ~(page - 1);
    mprotect((void*)start, end - start, PROT_READ|PROT_EXEC);
}

static uint32_t enc_movz_x17(uint32_t imm16) {
    return 0xD2800000u | ((imm16 & 0xFFFF) << 5) | 17u;
}
static uint32_t enc_b(uint64_t from, uint64_t to) {
    int64_t off = (int64_t)to - (int64_t)from;
    if (off < -(1LL<<27) || off >= (1LL<<27)) return 0;
    if (off & 3) return 0;
    return 0x14000000u | ((uint32_t)((off >> 2) & 0x3FFFFFFu));
}

// ---------------------------------------------------------------------------
// 生成站点 trampoline：把原指令搬过来，并处理好它的出口
//
// 四种情况：
//   1) 普通指令      : 原指令 ; 跳回 addr+4
//   2) ret           : 原指令（它自己走，不返回）
//   3) b target      : 绝对跳到 target（不返回）
//   4) b.cond/cbz/tbz target :
//        原条件指令 重定位到本地 .Ltaken
//        条件不成立 → 跳回 addr+4
//        .Ltaken   → 绝对跳到 target
//
// 返回写入的字节数；0 表示失败
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// wideMode 专用 trampoline：搬 4 条原指令，跳回 addr+16
//
// 为什么不能复用 build_trampoline：它只搬 1 条就跳回 addr+4，
// 而 16 字节入口把 addr+4 覆盖成了 `br x16` —— 跳回去就死循环。
//
// 前导（恢复 x16/x17/sp）只对指令级模式生成：只有它的 shell 里压了栈。
//   shell 里 stp 压 16 → sp = 原sp-16；handler sub 0x320；前导 add 0x330 补回。
// RAW 的 shell 不压栈，所以不需要前导。
//
// can_cover_16 已保证这 4 条不含分支/PC 相对/x16x17，可以原样复制。
// ---------------------------------------------------------------------------
static int build_trampoline_wide(void* tramp, unsigned long addr, int isLight) {
    uint32_t* t = (uint32_t*)tramp;
    int hdr = isLight ? 0 : 3;

    if (!isLight) {
        t[0] = 0xF9400000u | ((0x080u >> 3) << 10) | (31u << 5) | 16u;  // ldr x16,[sp,#0x80]
        t[1] = 0xF9400000u | ((0x088u >> 3) << 10) | (31u << 5) | 17u;  // ldr x17,[sp,#0x88]
        t[2] = 0x910003FFu | ((0x330u & 0xFFFu) << 10);                 // add sp,sp,#0x330
    }
    for (int i = 0; i < 4; ++i)
        t[hdr + i] = *(volatile uint32_t*)(addr + i*4);

    // 跳回 addr+16。trampoline 在独立 mmap（离目标远），只能绝对跳，
    // 会破坏 x16 —— can_cover_16 已保证原指令不用它。
    t[hdr + 4] = 0x58000050u;                       // ldr x16, #8
    t[hdr + 5] = 0xD61F0200u;                       // br  x16
    *(unsigned long*)(t + hdr + 6) = addr + 16;
    return (hdr + 8) * 4;
}

static int build_trampoline(void* tramp, uint32_t insn, unsigned long addr, int useRelJump, int isLight) {
    // 与 handler 约定：进入时 sp 指向 handler 栈，
    // 原 x16/x17 在 [sp+0x080] / [sp+0x088]，原 sp = sp + 0x330。
    uint32_t* t = (uint32_t*)tramp;
    uint64_t base = (uint64_t)tramp;

    int is_b    = ((insn & 0xFC000000) == 0x14000000);
    int is_cond = ((insn & 0xFF000010) == 0x54000000);
    int is_cbz  = ((insn & 0x7E000000) == 0x34000000);
    int is_tbz  = ((insn & 0x7E000000) == 0x36000000);
    int is_ret  = ((insn & 0xFFFFFC1F) == 0xD65F0000);

    // 公共前导：恢复 x16/x17、恢复 sp
    // ★ LIGHT 模式下 handler 自己已经恢复了 sp / x16 / x17，trampoline 不能再碰
    int hdr = isLight ? 0 : 3;
    if (!isLight) {
        t[0] = 0xF9400000u | ((0x080u >> 3) << 10) | (31u << 5) | 16u;  // ldr x16,[sp,#0x80]
        t[1] = 0xF9400000u | ((0x088u >> 3) << 10) | (31u << 5) | 17u;  // ldr x17,[sp,#0x88]
        t[2] = 0x910003FFu | ((0x330u & 0xFFFu) << 10);                 // add sp,sp,#0x330
    }

    // 后续所有 t[N] 都按 hdr 偏移
    int64_t back = (int64_t)(addr + 4) - (int64_t)(base + (hdr+1)*4);
    int backOK = (back > -(1LL<<27) && back < (1LL<<27) && !(back & 3));

    // 跳回方式：4 字节模式用相对 b（不碰寄存器）；
    //          16 字节模式 trampoline 在远处，只能用 ldr x16（后续代码已确认不用 x16）
    if (!useRelJump) backOK = 0;

    if (is_ret) {
        t[hdr] = insn;                 // ret 自己走
        return hdr*4 + 4;
    }
    if (is_b) {
        // 无条件跳：直接绝对跳过去
        int64_t off = (int64_t)(insn & 0x3FFFFFFu);
        if (off & 0x2000000) off -= 0x4000000;
        uint64_t target = (uint64_t)addr + (uint64_t)(off * 4);
        t[hdr] = 0x58000050u;          // ldr x16,#8
        t[1+hdr] = 0xD61F0200u;          // br  x16
        *(unsigned long*)(t + 2+hdr) = target;
        return hdr*4 + 16;
    }
    if (is_cond || is_cbz || is_tbz) {
        uint64_t target;
        if (!zh_get_target(insn, (uint64_t)addr, 0, 0, 0, &target)) return 0;
        if (!backOK) return 0;

        // t[hdr] 条件指令 → 跳到 t[4+hdr]（条件成立）
        // t[1+hdr] b addr+4（条件不成立）
        uint32_t cond = zh_set_target(insn, base + hdr*4, base + (4+hdr)*4);
        if (!cond) return 0;
        t[hdr] = cond;
        t[1+hdr] = 0x14000000u | ((uint32_t)((back >> 2) & 0x3FFFFFFu));
        t[2+hdr] = 0xD503201Fu;          // nop
        t[3+hdr] = 0xD503201Fu;          // nop
        t[4+hdr] = 0x58000050u;          // ldr x16,#8
        t[5+hdr] = 0xD61F0200u;          // br  x16
        *(unsigned long*)(t + 6+hdr) = target;
        return hdr*4 + 32;
    }

    // 普通指令
    t[hdr] = insn;
    if (backOK) {
        t[1+hdr] = 0x14000000u | ((uint32_t)((back >> 2) & 0x3FFFFFFu));
        return hdr*4 + 8;
    }
    // 远处 trampoline：绝对跳回，会破坏 x16（靠 next_n_clean 保证无害）
    t[1+hdr] = 0x58000050u;                                  // ldr x16, #8
    t[2+hdr] = 0xD61F0200u;                                  // br  x16
    *(unsigned long*)(t + 3+hdr) = (unsigned long)(addr + 4);
    return hdr*4 + 16;
}

// ---------------------------------------------------------------------------
// 安装
// ---------------------------------------------------------------------------
// ===========================================================================
// 站点原子操作
// ===========================================================================

// 4 字节指令原子替换 —— 不需要暂停任何线程。
// ARM64 对齐的 32 位写是原子的：跑到这里的线程要么已取指（继续按旧指令走），
// 要么还没到（下次取到新指令）。不存在半写状态。
// 之前 suspend_others() 暂停全部 300+ 线程会造成肉眼可见卡顿，已废弃。
static int patch32(unsigned long addr, uint32_t insn) {
    if (!make_writable(addr, 4)) return -1;
    *(volatile uint32_t*)addr = insn;
    __builtin___clear_cache((char*)addr, (char*)addr + 4);
    make_readable(addr, 4);
    return 0;
}

// 给已存在的站点记录装上桩：分配槽位 → 写 shell/trampoline → 原子改 b
// sid = 该站点在 g_sites 里的索引（handler 靠它找回回调）
// ---------------------------------------------------------------------------
// 重定向的 16 字节入口
//
// bl 只有 4 字节，用 b 跳只能 ±128MB。实测游戏 .so 附近 ±128MB 内没有
// 可用空洞，所以近处 shell 常常分配不到。此时改用 16 字节入口：
//
//   addr+0x00:  ldr  x16, #8        ; 0x58000050
//   addr+0x04:  br   x16            ; 0xD61F0200
//   addr+0x08:  .quad shell
//   （覆盖 addr..addr+16，原来的 4 条指令里 bl 被替换，
//     后面 3 条搬到 trampoline 里执行完再跳回 addr+16）
//
// shell 放在 mmap(NULL) 的任意地址即可，不再受 ±128MB 约束。
// ---------------------------------------------------------------------------
static void* redirect_shell_anywhere(void) {
    // 两个页：第一页放 shell 代码，第二页放 trampoline
    void* p = mmap(NULL, 8192, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
}

static int redirect_enable_wide(ii_site* s, int sid) {
    unsigned long addr = s->addr;

    // 被覆盖范围必须没有内部分支跳入
    for (unsigned long a = addr + 4; a < addr + 16; ++a) {
        if (!ii_can_instrument(a) && addr + 4 != addr) {
            // 只是保守检查：a 若不可插桩，说明 mid-instruction，
            // 但 16 字节对齐的 4 条指令本身是独立的，这里不强制。
            (void)0;
        }
    }

    void* base = redirect_shell_anywhere();
    if (!base) { wd_log("[redir_wide] mmap 失败\n"); return -15; }

    uint32_t* sh   = (uint32_t*)base;                 // shell
    uint32_t* tramp = (uint32_t*)((uint8_t*)base + 4096);  // trampoline

    // shell: stp x16,x17,[sp,#-16]! / movz x17,sid / ldr x16,#8 / br x16 / .quad handler
    sh[0] = 0xA9BF47F0u;
    sh[1] = enc_movz_x17((uint32_t)sid);
    sh[2] = 0x58000050u;
    sh[3] = 0xD61F0200u;
    *(unsigned long*)(sh + 4) = (unsigned long)ii_redirect_common;

    // trampoline: 目标函数返回后从这里继续。
    // 结构：A / B / C（原 addr+4..addr+0x10 三条指令）+ 跳 addr+16
    tramp[0] = *(volatile uint32_t*)(addr + 4);
    tramp[1] = *(volatile uint32_t*)(addr + 8);
    tramp[2] = *(volatile uint32_t*)(addr + 12);
    // 跳到 addr+16：ldr x16,#8 / br x16 / .quad addr+16
    tramp[3] = 0x58000050u;      // ldr x16, #8
    tramp[4] = 0xD61F0200u;      // br  x16
    // ldr 的 PC 在 tramp[3]（字节偏移 12），+8 → 偏移 20 = tramp[5]
    *(uint32_t*)(tramp + 5) = 0;                     // 对齐填充
    *(unsigned long*)((uint8_t*)tramp + 20) = (unsigned long)(addr + 16);

    __builtin___clear_cache((char*)base, (char*)base + 8192);

    // 16 字节非原子改写，必须暂停全部线程
    int stopped = suspend_others();
    if (stopped < g_stopExpect) {
        resume_others();
        zh_stop_wake_all();
        g_pauseDepth = 0; g_stopReq = 0;
        munmap(base, 8192);
        wd_log("[redir_wide] 暂停失败 %d/%d\n", stopped, g_stopExpect);
        return -17;
    }

    // 保存原始 16 字节用于恢复
    uint32_t* saved = (uint32_t*)malloc(16);
    if (!saved) {
        resume_others(); zh_stop_wake_all(); g_pauseDepth = 0; g_stopReq = 0;
        munmap(base, 8192);
        return -9;
    }
    memcpy(saved, (void*)addr, 16);

    uint32_t patch[4];
    patch[0] = 0x58000050u;
    patch[1] = 0xD61F0200u;
    *(unsigned long*)(patch + 2) = (unsigned long)sh;

    int ok = 0;
    if (make_writable(addr, 16)) {
        memcpy((void*)addr, patch, 16);
        __builtin___clear_cache((char*)addr, (char*)addr + 16);
        make_readable(addr, 16);
        uint32_t got = *(volatile uint32_t*)addr;
        ok = (got == patch[0]);
    }
    resume_others();
    zh_stop_wake_all();
    g_pauseDepth = 0; g_stopReq = 0;

    if (!ok) {
        free(saved);
        munmap(base, 8192);
        wd_log("[redir_wide] 写回校验失败\n");
        return -10;
    }

    s->shell      = (unsigned long)sh;
    s->wideMode   = 1;
    s->trampSaved = (unsigned long)saved;   // 16 字节原始内容
    s->trampoline = (unsigned long)base;    // ★ 存 mmap 起点，供 munmap 用
    s->installed  = 1;
    // 注意：真 trampoline 在 base+4096，卸载时 munmap(base, 8192) 一次全释放。
    wd_log("[redir_wide] OK addr=0x%lx shell=0x%lx tramp=0x%lx sid=%d\n",
           addr, (unsigned long)sh, (unsigned long)tramp, sid);
    return 0;
}

static int site_enable(ii_site* s, int sid) {
    unsigned long addr = s->addr;

    // ===== 调用重定向：与普通插桩完全不同的路径 =====
    // 原 bl 被改成 b shell（4 字节原子写），不搬原指令、不需要 trampoline。
    if (s->redirCb) {
        if (s->shell) {
            uint32_t* sh0 = (uint32_t*)s->shell;
            sh0[1] = enc_movz_x17((uint32_t)sid);
            __builtin___clear_cache((char*)s->shell, (char*)s->shell + 28);
        } else {
            int pi = -1;
            void* slot = alloc_shell_near(addr, &pi);

            // ★ 近处池拿不到时改走 16 字节 wideMode。
            //   bl 是 4 字节，用 b 只能跳 ±128MB；而实测游戏 .so 附近
            //   ±128MB 内连 64KB 空洞都没有（maps 里查过），近处 shell
            //   必然分配失败。
            //   16 字节入口 ldr x16,#8 / br x16 / .quad shell 能跳任意地址，
            //   代价是要搬走被覆盖的 3 条指令 + 暂停线程改写。
            if (!slot) {
                if (redirect_enable_wide(s, sid) == 0) return 0;
                return -15;
            }
            uint32_t* sh = (uint32_t*)slot;
            sh[0] = 0xA9BF47F0u;                              // stp x16,x17,[sp,#-16]!
            sh[1] = enc_movz_x17((uint32_t)sid);
            sh[2] = 0x58000050u;                              // ldr x16,#8
            sh[3] = 0xD61F0200u;                              // br x16
            *(unsigned long*)(sh + 4) = (unsigned long)ii_redirect_common;
            __builtin___clear_cache((char*)slot, (char*)slot + 28);
            s->shell   = (unsigned long)slot;
            s->poolIdx = pi;
        }
        uint32_t b = enc_b(addr, s->shell);
        if (!b) { wd_log("[redir_enable] enc_b 失败 addr=0x%lx shell=0x%lx\n", addr, s->shell); return -11; }
        if (patch32(addr, b) != 0) { wd_log("[redir_enable] patch32 失败\n"); return -10; }
        wd_log("[redir_enable] sid=%d addr=0x%lx shell=0x%lx b=0x%08x\n", sid, addr, s->shell, b);
        s->installed = 1;
        return 0;
    }

    // 复用之前分配过的槽位（禁用时不清除），避免每次启停都泄漏 64 字节
    if (s->shell) {
        uint32_t* sh = (uint32_t*)s->shell;
        if (!s->rawDetour) {
            sh[1] = enc_movz_x17((uint32_t)sid);    // sid 可能变，重写
            __builtin___clear_cache((char*)s->shell, (char*)s->shell + 28);
        }

        if (s->wideMode) {
            // 16 字节非原子改写必须暂停全部线程。
            // ★ 必须核对真的停了：suspend_others 有 300ms 超时，超时后继续改
            //   可能改到某个线程正在执行的指令上 → 崩溃。
            int stopped = suspend_others();
            if (stopped < g_stopExpect) {
                resume_others();
                wd_log("wideMode enable 放弃 @0x%lx：只暂停 %d/%d 个线程\n",
                       addr, stopped, g_stopExpect);
                return -17;
            }
            // 入口写法必须和 RAW / 指令级区分开：
            //   指令级：stp x16,x17,[sp,#-16]! + 跳到 shell（shell 内部会 ldp 回来）
            //   RAW  ：只做绝对跳转，绝不压栈 —— RAW 的 shell 不恢复 sp，
            //          压了就是栈错位 16 字节；而且 trampSaved 只存 16 字节，
            //          写 20 字节会让 site_disable 漏恢复第 5 条指令。
            // 统一 16 字节入口：ldr x16,#8 / br x16 / .quad shell
            uint32_t patch[4];
            patch[0] = 0x58000050u;
            patch[1] = 0xD61F0200u;
            *(unsigned long*)(patch + 2) = s->shell;
            if (make_writable(addr, 16)) {
                memcpy((void*)addr, patch, 16);
                __builtin___clear_cache((char*)addr, (char*)addr + 16);
                make_readable(addr, 16);
            }
            resume_others();
        } else {
            uint32_t b = enc_b(addr, s->shell);
            if (b == 0) return -11;
            if (patch32(addr, b) != 0) return -10;
        }
        s->installed = 1;
        return 0;
    }

    int poolIdx = -1;
    void* slot  = alloc_shell_near((uint64_t)addr, &poolIdx);
    void* shell = slot;
    void* tramp = slot ? (void*)((char*)slot + 32) : NULL;
    int wideMode = 0;

    if (slot) {
        int tlen = build_trampoline(tramp, s->orig, addr, 1, s->rawDetour != NULL || s->lightCb != NULL);
        if (tlen <= 0) { slot = NULL; shell = NULL; tramp = NULL; }
        else __builtin___clear_cache((char*)tramp, (char*)tramp + tlen);
    }

    if (!slot) {
        // 退 16 字节入口模式（±128MB 内找不到槽位时）
        //
        // 布局：patch 覆盖 4 条指令 = ldr x16,#8 / br x16 / .quad shell
        //       压栈交给 shell 做（只压一次），trampoline 搬 4 条后跳回 addr+16。
        //       早期版本把 stp 放进 patch 且 trampoline 只搬 1 条，sp 失衡 + 死循环。
        if (!can_cover_16(addr)) return -7;
        // 二次确认被搬走的 3 条（addr+4..addr+16）确实"干净"：
        //   next_n_clean 从 addr+4 开始扫，遇到 ret / b 就提前放行。
        //   can_cover_16 已经查过同样范围，这里是防御性重复 ——
        //   但参数必须是被覆盖的指令数 4，不能是原来的 32。
        //
        //   原值 32 的问题：它要求入口后 32 条指令全程不碰 x16/x17。
        //   而 AArch64 里 PLT 调用必然用 x16/x17，等于要求"函数前 32 条
        //   里不能有任何外部调用" —— 绝大多数函数满足不了，
        //   真机上 LocalPlayer_normalTick 就恒返回 -13。
        //   真正需要保证的只有"被搬走的那几条不碰 x16/x17"，
        //   因为 trampoline 末尾用 ldr x16 / br x16 跳回。
        if (!next_n_clean(addr, 4)) return -13;

        tramp = mmap(NULL, 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (tramp == MAP_FAILED) return -6;
        // wideMode 用专用构建：搬 4 条原指令，跳回 addr+16
        int tlen = build_trampoline_wide(tramp, addr, s->rawDetour != NULL || s->lightCb != NULL);
        if (tlen <= 0) { munmap(tramp, 4096); return -12; }
        __builtin___clear_cache((char*)tramp, (char*)tramp + tlen);

        shell = mmap(NULL, 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (shell == MAP_FAILED) { munmap(tramp, 4096); return -7; }
        wideMode = 1;
        if (!s->trampSaved) s->trampSaved = (unsigned long)malloc(16);
        if (s->trampSaved) memcpy((void*)s->trampSaved, (void*)addr, 16);
    }

    // 薄壳
    uint32_t* sh = (uint32_t*)shell;
    if (s->rawDetour) {
        // 直通模式：只有 16 字节绝对跳转，不动 sp，detour 拿到的是干净的现场。
        // 代价是 x16 被破坏（AAPCS64 的 IP0，入口处无值保证）。
        sh[0] = 0x58000050u;                     // ldr x16, #8
        sh[1] = 0xD61F0200u;                     // br  x16
        *(unsigned long*)(sh + 2) = (unsigned long)s->rawDetour;
        __builtin___clear_cache((char*)shell, (char*)shell + 16);
    } else {
        // 指令级：stp x16,x17,[sp,#-16]! ; mov x17,#sid ; ldr x16,#8 ; br x16 ; .quad handler
        sh[0] = 0xA9BF47F0u;
        sh[1] = enc_movz_x17((uint32_t)sid);
        sh[2] = 0x58000050u;
        sh[3] = 0xD61F0200u;
        *(unsigned long*)(sh + 4) = s->lightCb ? (unsigned long)ii_light : (unsigned long)ii_common;
        __builtin___clear_cache((char*)shell, (char*)shell + 28);
    }

    // 原子改写原地址
    int patchLen = wideMode ? 16 : 4;
    if (!make_writable(addr, patchLen)) {
        if (wideMode) { munmap(shell, 4096); munmap(tramp, 4096); }
        return -10;
    }
    if (wideMode) {
        // 16 字节非原子写 → 必须暂停全部线程
        int stopped = 0;
        for (int attempt = 0; attempt < 3; ++attempt) {
            stopped = suspend_others();
            if (stopped >= g_stopExpect) break;
            resume_others();
            g_pauseDepth = 0;
            usleep(20000);
        }
        if (stopped < g_stopExpect) {
            resume_others();
            wd_log("wideMode install 放弃 @0x%lx：3 次重试仍只暂停 %d/%d 个线程\n",
                   addr, stopped, g_stopExpect);
            make_readable(addr, 16);
            munmap(shell, 4096); munmap(tramp, 4096);
            return -17;
        }
        // 统一 16 字节：ldr x16,#8 / br x16 / .quad shell
        // 压栈交给 shell 做（只压一次），trampoline 前导按 0x330 补回。
        uint32_t patch[4];
        patch[0] = 0x58000050u;
        patch[1] = 0xD61F0200u;
        *(unsigned long*)(patch + 2) = (unsigned long)shell;
        memcpy((void*)addr, patch, 16);
        __builtin___clear_cache((char*)addr, (char*)addr + 16);
        resume_others();
    } else {
        uint32_t b = enc_b(addr, (uint64_t)shell);
        if (b == 0) { make_readable(addr, patchLen); return -11; }
        *(uint32_t*)addr = b;
    }
    __builtin___clear_cache((char*)addr, (char*)addr + patchLen);
    make_readable(addr, patchLen);

    s->shell      = (unsigned long)shell;
    s->trampoline = (unsigned long)tramp;
    s->wideMode   = wideMode;
    s->poolIdx    = wideMode ? -1 : poolIdx;
    s->installed  = 1;
    if (!wideMode && poolIdx >= 0) pool_note_site(poolIdx, addr);
    return 0;
}

// 拆掉一个站点（恢复原指令），保留记录以便随时再启用
static int site_disable(ii_site* s) {
    if (!s || !s->installed) return 0;
    unsigned long addr = s->addr;

    if (s->wideMode) {
        if (!s->trampSaved) {
            wd_log("wideMode disable @0x%lx：trampSaved 为空，无法恢复\n", addr);
            return -3;
        }
        // 16 字节入口：写非原子，必须暂停全部线程。
        // 停不全是常态（D 状态线程），重试几次再决定放弃。
        //
        // ★ 暂停/恢复必须严格配对。之前这里在重试分支里直接写
        //   g_pauseDepth = 0，跳过了 resume_others() 里的 zh_stop_wake_all()，
        //   线程还睡在信号处理器里没人唤醒；再叠加多实例连续 destroy，
        //   g_pauseDepth 泄漏后每次 resume 都只减计数不唤醒 ——
        //   真机上表现为全进程线程停在 do_signal_stop：整机卡死 + ANR。
        int stopped = -1;
        int suspended = 0;              // 1 = 当前确实处于暂停状态，需要 resume
        for (int attempt = 0; attempt < 3; ++attempt) {
            stopped = suspend_others();
            if (stopped < 0) { suspended = 0; break; }        // 直接失败，没暂停
            if (stopped >= g_stopExpect) { suspended = 1; break; }
            resume_others();            // 没停全：正常配对恢复后再重试
            suspended = 0;
            usleep(20000);              // 给 D 状态线程一点时间回到可中断状态
        }
        if (stopped < 0 || stopped < g_stopExpect) {
            if (suspended) resume_others();
            zh_stop_wake_all();         // 兜底：无论如何都要唤醒
            g_pauseDepth = 0;
            g_stopReq    = 0;
            wd_log("wideMode disable 放弃 @0x%lx：暂停失败 %d/%d 个线程\n",
                   addr, stopped, g_stopExpect);
            return -17;
        }
        int rc = -2;
        int wrote = 0;
        if (make_writable(addr, 16)) {
            for (int i = 0; i < 4; ++i)
                *(uint32_t*)(addr + i*4) = *(volatile uint32_t*)(s->trampSaved + i*4);
            __builtin___clear_cache((char*)addr, (char*)addr + 16);
            make_readable(addr, 16);
            wrote = 1;
            // 校验真的写回去了（W^X 下 make_writable 偶发失败会被这里抓住）
            uint32_t want0 = *(volatile uint32_t*)(s->trampSaved);
            uint32_t got0  = *(volatile uint32_t*)addr;
            if (got0 != want0) {
                wd_log("wideMode disable @0x%lx：写回校验失败 got=%08x want=%08x\n",
                       addr, got0, want0);
                rc = -4;
            } else {
                rc = 0;
            }
        } else {
            wd_log("wideMode disable @0x%lx：make_writable(16) 失败 errno=%d\n", addr, errno);
        }
        (void)wrote;
        // 统一出口：先恢复线程，再看结果
        if (suspended) resume_others();
        zh_stop_wake_all();             // 兜底
        g_pauseDepth = 0;
        g_stopReq    = 0;
        if (rc != 0) return rc;
    } else {
        // 4 字节：原子写，零卡顿
        if (patch32(addr, s->orig) != 0) return -2;
    }
    s->installed = 0;
    return 0;
}

// 站点在 g_sites 里的索引（= handler 用的 sid）
static int site_id(ii_site* s) {
    for (int i = 0; i < g_nSites; ++i)
        if (site_at(i) == s) return i;
    return -1;
}

// ===========================================================================
// 安装
// ===========================================================================
int ii_install(unsigned long addr, ii_callback cb, void* user) {
    return ii_install_ex(addr, cb, user, II_PRIO_DEFAULT);
}

int ii_install_ex(unsigned long addr, ii_callback cb, void* user, int priority) {
    if (!addr || !cb) return -1;

    // 同一地址可挂多个回调（对标 Mixin 的多个 @Inject）。
    // 已存在时追加到回调链，而不是返回 -2。
    ii_site* existing = ii_find(addr);
    if (existing) {
        if (existing->lightCb) return -16;   // LIGHT 站点不支持多回调
        return chain_insert(existing, cb, user, priority);
    }

    if (g_nSites >= II_MAX_SITES) return -3;
    if (ii_init() != 0) return -4;

    // 跨探针共存：别的探针已经在这里插过桩 → 挂到它的回调链上，而不是拒绝。
    //
    // 不能重复 patch：否则我们读到的"原指令"其实是别人的 b，将来 disable
    // 时会把那条 b 写回原地址，给上一个探针留下指向失效 shell 的跳转。
    // 但也不能拒绝——多探针共存是常态。
    //
    // handler 类型必须一致：FULL 的 ii_ctx 与 LIGHT 的 ii_ctx_light 布局不同，
    // 混挂会让回调读到错位的寄存器。
    if (g_reg) {
        int zidx = zhreg_find_site(g_reg, addr);
        if (zidx >= 0) {
            if (g_reg->site[zidx].kind != 0) return -16;   // 已有的是 LIGHT 站点
            int rc = zhreg_add_cb(g_reg, zidx, (uint64_t)cb, (uint64_t)user, g_myId, II_PRIO_DEFAULT);
            if (rc < 0) return rc;
            // 共享站点也要在本实例记账：否则 ii_engine_count() 显示 0，
            // 用户会以为安装失败（t11 在真机上就是这个现象）。
            ii_engine* me = ii_default();
            if (me) me->nSites++;
            wd_log("install 共享 @0x%lx：挂到 owner=%d 的回调链，槽 %d\n",
                   addr, g_reg->site[zidx].owner, rc);
            return 0;
        }
    }

    if (!ii_can_instrument(addr)) return -5;

    ii_site* s = calloc(1, sizeof(ii_site));
    if (!s) return -9;

    s->addr    = addr;
    s->orig    = *(volatile uint32_t*)addr;
    // 回调走链表：支持同一地址挂多个（对标 Mixin 多个 @Inject）
    if (chain_insert(s, cb, user, priority) != 0) { free(s); return -9; }
    s->user    = user;
    s->enabled = 1;
    s->pending = 0;
    s->regIdx  = -1;
    s->retKind = ii_ret_kind(addr);
    s->priority = priority;

    int sid = site_append(s);       // 先登记，handler 才能查到
    if (sid < 0) { chain_clear(s); free(s); return -3; }

    int rc = site_enable(s, sid);
    if (rc != 0) { site_set(sid, NULL); chain_clear(s); free(s); return rc; }

    if (g_reg) {
        s->regIdx = zhreg_add_site(g_reg, addr, s->orig, s->poolIdx, g_myId, 0 /*FULL*/);
        // 自己的回调也进链：别的探针的 handler 要靠它找到我们
        if (s->regIdx >= 0)
            zhreg_add_cb(g_reg, s->regIdx, (uint64_t)cb, (uint64_t)user, g_myId, s->priority);
    }
    return 0;
}

// ===========================================================================
// 轻量安装（高频函数专用）
// ===========================================================================
int ii_install_light(unsigned long addr, ii_light_callback cb, void* user) {
    return ii_install_light_ex(addr, cb, user, II_PRIO_DEFAULT);
}

int ii_install_light_ex(unsigned long addr, ii_light_callback cb, void* user, int priority) {
    if (!addr || !cb) return -1;
    if (ii_find(addr)) return -2;
    if (g_nSites >= II_MAX_SITES) return -3;
    if (ii_init() != 0) return -4;

    // 跨探针共存：已有站点就挂到它的链上（LIGHT 必须配 LIGHT 站点）
    if (g_reg) {
        int zidx = zhreg_find_site(g_reg, addr);
        if (zidx >= 0) {
            if (g_reg->site[zidx].kind != 1) return -16;   // 已有的是 FULL 站点
            int rc2 = zhreg_add_cb(g_reg, zidx, (uint64_t)cb, (uint64_t)user, g_myId, II_PRIO_DEFAULT);
            if (rc2 < 0) return rc2;
            wd_log("install_light 共享 @0x%lx：槽 %d\n", addr, rc2);
            return 0;
        }
    }

    if (!ii_can_instrument(addr)) return -5;

    ii_site* s = calloc(1, sizeof(ii_site));
    if (!s) return -9;
    s->addr    = addr;
    s->orig    = *(volatile uint32_t*)addr;
    s->lightCb = cb;
    s->user    = user;
    s->enabled = 1;
    s->pending = 0;
    s->regIdx  = -1;      // ★ calloc 给的是 0，而 0 是合法站点索引，必须显式置 -1
    s->retKind = ii_ret_kind(addr);
    s->priority = priority;

    int sid = site_append(s);
    if (sid < 0) { chain_clear(s); free(s); return -3; }

    int rc = site_enable(s, sid);
    if (rc != 0) { site_set(sid, NULL); chain_clear(s); free(s); return rc; }
    if (g_reg) {
        s->regIdx = zhreg_add_site(g_reg, addr, s->orig, s->poolIdx, g_myId, 1 /*LIGHT*/);
        if (s->regIdx >= 0)
            zhreg_add_cb(g_reg, s->regIdx, (uint64_t)cb, (uint64_t)user, g_myId, s->priority);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 直通模式：detour 完全接管函数入口（对应 HookEngine::installAt）
//
// 与指令级的区别见头文件。这里的关键点：
//   - shell 只写 16 字节绝对跳转，不动 sp、不压栈 → detour 看到的是干净现场
//   - trampoline 不需要前导（没有 sp 要恢复），hdr=0
//   - 不支持回调链：函数级 hook 是「替换」语义，两个 detour 无法叠加。
//     别的探针已占用该地址时返回 -14（指令级模式才走链）
// ---------------------------------------------------------------------------
unsigned long ii_redirect_orig_target(unsigned long callSiteAddr) {
    ii_site* s = ii_find(callSiteAddr);
    return (s && s->redirCb) ? s->origTarget : 0;
}

// 调用重定向安装
int ii_redirect_call(unsigned long callSiteAddr, unsigned long origTarget,
                     ii_redirect_cb cb, void* user) {
    if (!callSiteAddr || !origTarget || !cb) return -1;
    if (ii_find(callSiteAddr)) return -2;
    if (g_nSites >= II_MAX_SITES) return -3;
    if (ii_init() != 0) return -4;

    // 必须是 bl 指令
    uint32_t insn = *(volatile uint32_t*)callSiteAddr;
    if ((insn & 0xFC000000) != 0x94000000) return -5;

    // 目标地址必须是可执行内存（防止把 b 跳到数据段）
    uint64_t gp = zh_game_base();
    (void)gp;

    ii_site* s = calloc(1, sizeof(ii_site));
    if (!s) return -9;
    s->addr       = callSiteAddr;
    s->orig       = insn;
    s->callSite   = callSiteAddr;
    s->origTarget = origTarget;
    s->redirCb    = (void*)cb;
    s->user       = user;
    s->enabled    = 1;
    s->regIdx     = -1;
    s->retKind    = ii_ret_kind(origTarget);
    s->priority   = II_PRIO_DEFAULT;

    int sid = site_append(s);
    if (sid < 0) { chain_clear(s); free(s); return -3; }

    int rc = site_enable(s, sid);
    if (rc != 0) { site_set(sid, NULL); chain_clear(s); free(s); return rc; }

    if (g_reg) {
        // 站点号 kind 用 3 表示 redirect，便于诊断
        s->regIdx = zhreg_add_site(g_reg, callSiteAddr, insn, s->poolIdx, g_myId, 3);
    }
    return 0;
}

// 按函数名安装（用 blcall_db 查调用点地址）
int ii_redirect_name(const char* hostFunc, const char* targetFunc, int nth,
                     ii_redirect_cb cb, void* user) {
    unsigned long callAddr = 0, targetAddr = 0;
    int rc = ii_call_site_addr(hostFunc, targetFunc, nth, &callAddr);
    if (rc != 0) return rc;
    // 目标绝对地址 = base + 目标的 vaddr
    uint64_t base = zh_game_base();
    if (!base) return -2;
    // blcall_db 里每条都带 targetOff，但 ii_call_site_addr 没返回它，这里查表
    int nbc = zh_tbl_count(ZH_TBL_BLCALL);
    for (int i = 0; i < nbc; ++i) {
        const char *bf, *bt; uint32_t boff, btoff;
        if (zh_tbl_blcall(i, &bf, &boff, &bt, &btoff) != 0) continue;
        if (!bf || !bt) continue;
        if (__builtin_strcmp(bf, hostFunc)) continue;
        if (__builtin_strcmp(bt, targetFunc)) continue;
        (void)boff;
        targetAddr = base + btoff;
        break;
    }
    if (!targetAddr) return -6;
    return ii_redirect_call(callAddr, targetAddr, cb, user);
}

int ii_install_raw(unsigned long addr, void* detour, void** origOut) {
    if (origOut) *origOut = NULL;
    if (!addr || !detour) return -1;
    if (ii_find(addr)) return -2;
    if (g_nSites >= II_MAX_SITES) return -3;
    if (ii_init() != 0) return -4;

    // 替换语义无法叠加：任何探针（含指令级站点）占了这个地址就拒绝
    if (g_reg && zhreg_find_site(g_reg, addr) >= 0) return -14;

    if (!ii_can_instrument(addr)) return -5;

    ii_site* s = calloc(1, sizeof(ii_site));
    if (!s) return -9;
    s->addr      = addr;
    s->orig      = *(volatile uint32_t*)addr;
    s->rawDetour = detour;
    s->enabled   = 1;
    s->pending   = 0;
    s->regIdx    = -1;
    s->retKind   = ii_ret_kind(addr);
    s->priority  = II_PRIO_DEFAULT;

    int sid = site_append(s);
    if (sid < 0) { chain_clear(s); free(s); return -3; }

    int rc = site_enable(s, sid);
    if (rc != 0) { site_set(sid, NULL); chain_clear(s); free(s); return rc; }

    if (g_reg) {
        s->regIdx = zhreg_add_site(g_reg, addr, s->orig, s->poolIdx, g_myId, 2 /*RAW*/);
    }
    if (origOut) *origOut = (void*)s->trampoline;
    return 0;
}

// ===========================================================================
// 返回值类型校验（B1）
//
// rettype_db.h 里每个函数都标了 kind + 可读类型名。这里把「绝对地址 → 类型」
// 打通，回调就能在设返回值前确认不会把 int 写到 float 返回的函数上。
// ===========================================================================
// 返回值类型查询（走表抽象层，编译期表与运行时表都支持）
static int zh_ret_lookup_kind(uint64_t addr, int* kind, const char** typeName) {
    uint64_t base = zh_game_base();
    if (!base || addr < base) return -1;
    uint32_t off = (uint32_t)(addr - base);
    return zh_tbl_rettype_by_off(off, kind, typeName);
}

int ii_site_priority(unsigned long addr) {
    ii_site* s = ii_find(addr);
    return s ? s->priority : 0;
}

int ii_set_site_priority(unsigned long addr, int priority) {
    ii_site* s = ii_find(addr);
    if (!s) return -1;
    s->priority = priority;
    return 0;
}

int ii_ret_kind(unsigned long addr) {
    int k = II_RET_UNKNOWN;
    if (zh_ret_lookup_kind((uint64_t)addr, &k, NULL) != 0) return II_RET_UNKNOWN;
    return k;
}

int ii_ret_kind_name(const char* funcName) {
    if (!funcName) return II_RET_UNKNOWN;
    int k = II_RET_UNKNOWN;
    if (zh_tbl_rettype_by_name(funcName, &k, NULL) != 0) return II_RET_UNKNOWN;
    return k;
}

const char* ii_ret_type_name(unsigned long addr) {
    const char* tn = NULL;
    if (zh_ret_lookup_kind((uint64_t)addr, NULL, &tn) != 0 || !tn) return "unknown";
    return tn;
}

// 返回值落在哪个寄存器：bool/int/ptr → x0；float → s0；void/struct → 不动
// ===========================================================================
// 字段访问器（B4）
// ===========================================================================
// ===========================================================================
// 函数内调用点（B3，部分对标 @Redirect）
// ===========================================================================
// ===========================================================================
// 常量修改（@ModifyConstant 等价）
// ===========================================================================
static int zh_const_lookup(const char* funcName, uint32_t value, int nth,
                           uint32_t* offOut, uint8_t* kindOut) {
    if (!funcName || nth < 0) return -1;
    int seen = 0;
    int fidx = zh_tbl_constfunc_index(funcName);
    if (fidx < 0) return -3;
    int nc = zh_tbl_count(ZH_TBL_CONST);
    for (int i = 0; i < nc; ++i) {
        int fi, kind; uint32_t io, val;
        zh_tbl_const(i, &fi, &kind, NULL, &io, &val);
        if (fi != fidx) continue;
        if (val != value) continue;
        if (seen++ != nth) continue;
        if (offOut) *offOut = io;
        if (kindOut) *kindOut = (uint8_t)kind;
        return 0;
    }
    return -4;
}

// ===========================================================================
// 参数类型推断
//
// 只做「有把握」的推断：函数名的所属类前缀 + ABI 的粗分类。
// 拿不准就返回 "ptr"，绝不编造具体类型 —— 错的类型名会造成误用。
// ===========================================================================
// 注意：这里只处理函数名本身，不额外分配内存
//
// ★ 这里不能用 __thread（会产生 TLSDESC 重定位，bionic 拒绝加载）。
// 改用「每次调用返回指向调用方缓冲的指针」的写法，
// 由 ii_owner_class_r() 提供线程安全版本；ii_owner_class() 保留但标记为非线程安全。
static char g_ownerBuf[64];

const char* ii_owner_class(const char* funcName) {
    if (!funcName) return "";
    // 取第一个 '_' 之前的部分作为类名；纯 snake_case 的函数（无下划线）返回空
    const char* us = funcName;
    while (*us && *us != '_') ++us;
    if (!*us || us == funcName) return "";
    size_t n = (size_t)(us - funcName);
    if (n >= sizeof(g_ownerBuf)) return "";
    __builtin_memcpy(g_ownerBuf, funcName, n);
    g_ownerBuf[n] = 0;
    // 过滤掉明显不是类名的前缀（如 get/set/is 开头的自由函数）
    static const char* bad[] = {"get","set","is","has","can","do","on","update","tick",0};
    for (int i = 0; bad[i]; ++i)
        if (__builtin_strlen(bad[i]) == n && !__builtin_strncmp(g_ownerBuf, bad[i], n))
            return "";
    return g_ownerBuf;
}

const char* ii_arg_type(const char* funcName, int argIdx) {
    if (!funcName || argIdx < 0 || argIdx > 7) return "ptr";
    // 返回值类型表里能查到 kind 的话，用它判断这个函数是否「无参数」
    // 参数个数信息在 abifinal_real.h 里，这里用简化规则：
    //   argIdx == 0 且函数名有类前缀 → "<Class>*"（this 指针）
    //   其余 → 按 abifinal 的 xKind 判断（无该表则保守返回 "ptr"）
    if (argIdx == 0) {
        const char* cls = ii_owner_class(funcName);
        if (cls[0]) {
            static char buf[80];
            __builtin_snprintf(buf, sizeof(buf), "%s*", cls);
            return buf;
        }
    }
    return "ptr";
}

static uint32_t zh_name_to_off(const char* name);   // 定义在 B3 段

int ii_const_count(const char* funcName, uint32_t value) {
    if (!funcName) return 0;
    int n = 0;
    int fidx = zh_tbl_constfunc_index(funcName);
    if (fidx < 0) return 0;
    int nc = zh_tbl_count(ZH_TBL_CONST);
    for (int i = 0; i < nc; ++i) {
        int fi; uint32_t val;
        zh_tbl_const(i, &fi, NULL, NULL, NULL, &val);
        if (fi == fidx && val == value) ++n;
    }
    return n;
}

int ii_const_addr(const char* funcName, uint32_t value, int nth, unsigned long* outAddr) {
    if (outAddr) *outAddr = 0;
    uint32_t off = 0;
    int rc = zh_const_lookup(funcName, value, nth, &off, NULL);
    if (rc != 0) return rc;
    uint64_t base = zh_game_base();
    if (!base) return -2;
    uint32_t hostOff = zh_name_to_off(funcName);
    if (!hostOff) return -3;
    if (outAddr) *outAddr = base + hostOff + off;
    return 0;
}

int ii_install_at_const(const char* funcName, uint32_t value, int nth,
                        ii_callback cb, void* user) {
    unsigned long a = 0;
    int rc = ii_const_addr(funcName, value, nth, &a);
    if (rc != 0 || !a) return rc ? rc : -5;
    return ii_install(a, cb, user);
}

// ===========================================================================
// 返回点 / 函数末尾（@Inject(RETURN) / TAIL 等价）
// ===========================================================================
int ii_retpoint_total(void) { return N_RETPOINT; }

static int zh_retpoint_lookup(const char* funcName, int nth, uint32_t* offOut, int* isLast) {
    // nth == -1 是 TAIL（最后一个 ret）的合法取值，不能当非法值拒掉
    if (!funcName || nth < -1) return -1;
    int fidx = zh_tbl_retfunc_index(funcName);
    if (fidx < 0) return -3;
    int nrp = zh_tbl_count(ZH_TBL_RETPOINT);

    if (nth == -1) {          // -1 = 取 TAIL（最后一个）
        int best = -1; uint32_t bo = 0;
        for (int i = 0; i < nrp; ++i) {
            int fi; uint32_t ro;
            zh_tbl_retpoint(i, &fi, NULL, &ro);
            if (fi == fidx) { best = i; bo = ro; }
        }
        if (best < 0) return -4;
        if (offOut) *offOut = bo;
        if (isLast) *isLast = 1;
        return 0;
    }
    int seen = 0;
    for (int i = 0; i < nrp; ++i) {
        int fi, lz; uint32_t ro;
        zh_tbl_retpoint(i, &fi, &lz, &ro);
        if (fi != fidx) continue;
        if (seen++ != nth) continue;
        if (offOut) *offOut = ro;
        if (isLast) *isLast = lz;
        return 0;
    }
    return -4;
}

int ii_retpoint_count(const char* funcName) {
    if (!funcName) return 0;
    int fidx = zh_tbl_retfunc_index(funcName);
    if (fidx < 0) return 0;
    int n = 0, nrp = zh_tbl_count(ZH_TBL_RETPOINT);
    for (int i = 0; i < nrp; ++i) {
        int fi;
        zh_tbl_retpoint(i, &fi, NULL, NULL);
        if (fi == fidx) ++n;
    }
    return n;
}

int ii_retpoint_addr(const char* funcName, int nth, unsigned long* outAddr) {
    if (outAddr) *outAddr = 0;
    uint32_t off = 0;
    int rc = zh_retpoint_lookup(funcName, nth, &off, NULL);
    if (rc != 0) return rc;
    uint64_t base = zh_game_base();
    if (!base) return -2;
    uint32_t hostOff = zh_name_to_off(funcName);
    if (!hostOff) return -3;
    if (outAddr) *outAddr = base + hostOff + off;
    return 0;
}

int ii_install_at_return(const char* funcName, int nth, ii_callback cb, void* user) {
    unsigned long a = 0;
    int rc = ii_retpoint_addr(funcName, nth, &a);
    if (rc != 0 || !a) return rc ? rc : -5;
    // ret 本身不能插桩（trampoline 无法搬移），退到前一条指令：
    // 此时返回值已在 x0/s0 里，回调可以读也可以改。
    return ii_install(a - 4, cb, user);
}

int ii_install_at_tail(const char* funcName, ii_callback cb, void* user) {
    unsigned long a = 0;
    int rc = ii_retpoint_addr(funcName, -1, &a);
    if (rc != 0 || !a) return rc ? rc : -5;
    return ii_install(a - 4, cb, user);
}

// ===========================================================================
// 局部变量访问（@Local 的部分替代）
// ===========================================================================
int64_t ii_local_read(ii_ctx* c, uint16_t spOff, uint8_t size) {
    if (!c || !c->sp) return 0;
    const volatile uint8_t* p = (const volatile uint8_t*)(c->sp + spOff);
    switch (size) {
        case 1: { int8_t  v; __builtin_memcpy((void*)&v, (const void*)p, 1); return v; }
        case 2: { int16_t v; __builtin_memcpy((void*)&v, (const void*)p, 2); return v; }
        case 4: { int32_t v; __builtin_memcpy((void*)&v, (const void*)p, 4); return v; }
        case 8: { int64_t v; __builtin_memcpy((void*)&v, (const void*)p, 8); return v; }
        default: return 0;
    }
}

void ii_local_write(ii_ctx* c, uint16_t spOff, uint8_t size, int64_t v) {
    if (!c || !c->sp) return;
    volatile uint8_t* p = (volatile uint8_t*)(c->sp + spOff);
    switch (size) {
        case 1: { int8_t  t = (int8_t)v;  __builtin_memcpy((void*)p, &t, 1); break; }
        case 2: { int16_t t = (int16_t)v; __builtin_memcpy((void*)p, &t, 2); break; }
        case 4: { int32_t t = (int32_t)v; __builtin_memcpy((void*)p, &t, 4); break; }
        case 8: { int64_t t = v;          __builtin_memcpy((void*)p, &t, 8); break; }
        default: break;
    }
}

// 找函数的栈帧信息（走表抽象层）
static int zh_frame_find(const char* funcName, uint32_t* frameSize,
                         uint32_t* slotStart, int* slotCount) {
    if (!funcName) return -1;
    return zh_tbl_frame_by_name(funcName, frameSize, slotStart, slotCount);
}

// ===========================================================================
// .eh_frame CFI：精确栈帧布局
//
// 静态扫描 sub sp/#N 会被函数后半段的冷分支骗（实测 ItemStack_isNull
// 扫出 0x440，真实只有 0x20）。CFI 是编译器逐指令记录的，准确。
//
// 数据由 parse_ehframe.py 从 .eh_frame 解析、gen_frame_cfi.py 转成紧凑二进制，
// 运行期 mmap 加载（避免把 1.7 MB 编进 .so）。
// ===========================================================================

int ii_frame_cfi_load(const char* path) {
    if (!path) return -1;
    if (g_cfiBase) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -2;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 32) { close(fd); return -3; }
    void* m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return -4;
    const uint8_t* b = (const uint8_t*)m;
    if (__builtin_memcmp(b, "ZHCFI\0\0\0", 8) != 0) { munmap(m, (size_t)st.st_size); return -5; }
    uint32_t cnt, ver;
    __builtin_memcpy(&cnt, b + 8, 4);
    __builtin_memcpy(&ver, b + 12, 4);
    if (ver != 1) { munmap(m, (size_t)st.st_size); return -6; }
    g_cfiBase = b; g_cfiLen = (size_t)st.st_size; g_cfiCount = cnt;
    return 0;
}

int ii_frame_cfi_loaded(void) { return g_cfiBase ? (int)g_cfiCount : 0; }

int ii_frame_cfi(uint32_t funcOff, uint8_t* cfaRegOut, int16_t* cfaOffOut, uint8_t* nregOut) {
    if (!g_cfiBase || !g_cfiCount) return -1;
    uint32_t lo = 0, hi = g_cfiCount;
    while (lo < hi) {                      // 条目按 funcOff 升序，二分
        uint32_t mid = (lo + hi) / 2;
        uint32_t off;
        __builtin_memcpy(&off, g_cfiBase + 16 + (size_t)mid * 8, 4);
        if (off < funcOff) lo = mid + 1;
        else if (off > funcOff) hi = mid;
        else {
            const uint8_t* e = g_cfiBase + 16 + (size_t)mid * 8;
            uint8_t  cr = e[4];
            int16_t  co;
            __builtin_memcpy(&co, e + 5, 2);
            if (cfaRegOut) *cfaRegOut = cr;
            if (cfaOffOut) *cfaOffOut = co;
            if (nregOut) *nregOut = e[7];
            return 0;
        }
    }
    return -2;
}

int ii_frame_size(const char* funcName) {
    // 优先 CFI（精确）：它给出的是「栈帧最深时 CFA 相对 x29/sp 的偏移」
    uint32_t fnOff = zh_name_to_off(funcName);
    if (fnOff && g_cfiBase) {
        uint8_t cr = 0; int16_t co = 0; uint8_t nr = 0;
        if (ii_frame_cfi(fnOff, &cr, &co, &nr) == 0 && co > 0) return (int)co;
    }
    // 退回静态扫描（粗糙，会被冷分支骗）
    uint32_t fsz = 0;
    if (zh_frame_find(funcName, &fsz, NULL, NULL) != 0) return 0;
    return (int)fsz;
}

int ii_local_count(const char* funcName) {
    int scnt = 0;
    if (zh_frame_find(funcName, NULL, NULL, &scnt) != 0) return 0;
    return scnt;
}

int ii_local_slot(const char* funcName, int idx,
                  uint16_t* spOffOut, uint8_t* sizeOut, uint32_t* insnOffOut) {
    uint32_t fsz, sstart; int scnt;
    if (zh_frame_find(funcName, &fsz, &sstart, &scnt) != 0) return -1;
    if (idx < 0 || idx >= scnt) return -1;
    uint16_t so; int sz; uint32_t io;
    if (zh_tbl_slot((int)(sstart + (uint32_t)idx), &so, &sz, &io) != 0) return -1;
    if (spOffOut) *spOffOut = so;
    if (sizeOut) *sizeOut = (uint8_t)sz;
    if (insnOffOut) *insnOffOut = io;
    return 0;
}

int ii_call_site_total(void) { return N_BLCALL; }

// 按名字查函数 vaddr 偏移（走表抽象层）
static uint32_t zh_name_to_off(const char* name) {
    if (!name) return 0;
    int n = zh_tbl_count(ZH_TBL_RETTYPE);
    for (int i = 0; i < n; ++i) {
        const char* fn; uint32_t off;
        zh_tbl_rettype(i, &fn, &off, NULL, NULL);
        if (fn && !__builtin_strcmp(fn, name)) return off;
    }
    return 0;
}

int ii_call_site_count(const char* funcName, const char* targetName) {
    if (!funcName || !targetName) return 0;
    int n = 0;
    int nbc = zh_tbl_count(ZH_TBL_BLCALL);
    for (int i = 0; i < nbc; ++i) {
        const char *bf, *bt;
        zh_tbl_blcall(i, &bf, NULL, &bt, NULL);
        if (bf && bt && !__builtin_strcmp(bf, funcName) &&
            !__builtin_strcmp(bt, targetName)) ++n;
    }
    return n;
}

int ii_call_site_addr(const char* funcName, const char* targetName,
                      int nth, unsigned long* outAddr) {
    if (outAddr) *outAddr = 0;
    if (!funcName || !targetName) return -1;
    // nth < 0 表示"该组合的第一个调用点"。
    // zh_redirect() 传的就是 -1（它不区分多个调用点），
    // 之前这里直接拒绝负数，导致 zh_redirect 永远返回 -1（t12 真机现象）。
    if (nth < 0) nth = 0;
    uint64_t base = zh_game_base();
    if (!base) return -2;
    uint32_t hostOff = zh_name_to_off(funcName);
    if (!hostOff) return -3;
    int seen = 0;
    int nbc = zh_tbl_count(ZH_TBL_BLCALL);
    for (int i = 0; i < nbc; ++i) {
        const char *bf, *bt; uint32_t boff, btoff;
        if (zh_tbl_blcall(i, &bf, &boff, &bt, &btoff) != 0) continue;
        if (!bf || !bt) continue;
        if (__builtin_strcmp(bf, funcName)) continue;
        if (__builtin_strcmp(bt, targetName)) continue;
        if (seen++ != nth) continue;
        (void)btoff;
        if (outAddr) *outAddr = base + hostOff + boff;
        return 0;
    }
    return -4;
}

int ii_install_before_call(const char* funcName, const char* targetName,
                           int nth, ii_callback cb, void* user) {
    unsigned long callAddr = 0;
    int rc = ii_call_site_addr(funcName, targetName, nth, &callAddr);
    if (rc != 0 || !callAddr) return rc ? rc : -5;
    // bl 本身不能插桩（trampoline 无法搬移跳转指令），退到前一条指令。
    // 因此这是「观察」而非「重定向」：能知道即将调用谁、参数是什么，
    // 但不能把这次调用换掉。
    return ii_install(callAddr - 4, cb, user);
}

int64_t ii_field_read(ii_ctx* c, uint8_t argIdx, uint16_t off, uint8_t size) {
    if (!c || argIdx >= 8) return 0;
    uint64_t base = c->x[argIdx];
    if (!base) return 0;
    volatile uint8_t* p = (volatile uint8_t*)(base + off);
    switch (size) {
        case 1: { int8_t  v; __builtin_memcpy((void*)&v, (const void*)p, 1); return v; }
        case 2: { int16_t v; __builtin_memcpy((void*)&v, (const void*)p, 2); return v; }
        case 4: { int32_t v; __builtin_memcpy((void*)&v, (const void*)p, 4); return v; }
        case 8: { int64_t v; __builtin_memcpy((void*)&v, (const void*)p, 8); return v; }
        default: return 0;
    }
}

void ii_field_write(ii_ctx* c, uint8_t argIdx, uint16_t off, uint8_t size, int64_t v) {
    if (!c || argIdx >= 8) return;
    uint64_t base = c->x[argIdx];
    if (!base) return;
    volatile uint8_t* p = (volatile uint8_t*)(base + off);
    switch (size) {
        case 1: { int8_t  t = (int8_t)v;  __builtin_memcpy((void*)p, &t, 1); break; }
        case 2: { int16_t t = (int16_t)v; __builtin_memcpy((void*)p, &t, 2); break; }
        case 4: { int32_t t = (int32_t)v; __builtin_memcpy((void*)p, &t, 4); break; }
        case 8: { int64_t t = v;          __builtin_memcpy((void*)p, &t, 8); break; }
        default: break;
    }
}

int ii_field_count(const char* funcName) {
    if (!funcName) return 0;
    int n = 0;
    int nf = zh_tbl_count(ZH_TBL_FIELDACC);
    for (int i = 0; i < nf; ++i) {
        const char* fn;
        if (zh_tbl_fieldacc(i, &fn, NULL, NULL, NULL, NULL) != 0) continue;
        if (fn && !__builtin_strcmp(fn, funcName)) ++n;
    }
    return n;
}

int ii_field_at(const char* funcName, int idx,
                uint16_t* offOut, uint8_t* baseOut, uint8_t* sizeOut, uint8_t* wrOut) {
    if (!funcName || idx < 0) return -1;
    int seen = 0;
    int nf = zh_tbl_count(ZH_TBL_FIELDACC);
    for (int i = 0; i < nf; ++i) {
        const char* fn; uint16_t o; int b, sz, w;
        if (zh_tbl_fieldacc(i, &fn, &o, &b, &sz, &w) != 0) continue;
        if (!fn || __builtin_strcmp(fn, funcName)) continue;
        if (seen++ != idx) continue;
        if (offOut)  *offOut  = o;
        if (baseOut) *baseOut = (uint8_t)b;
        if (sizeOut) *sizeOut = (uint8_t)sz;
        if (wrOut)   *wrOut   = (uint8_t)w;
        return 0;
    }
    return -2;
}

int ii_ret_reg(unsigned long addr) {
    int kind = II_RET_UNKNOWN;
    if (zh_ret_lookup_kind((uint64_t)addr, &kind, NULL) != 0) return 0;
    switch (kind) {
        case ZR_BOOL: case ZR_INT: case ZR_PTR: return 1;   // x0
        case ZR_FLOAT: return 2;                            // s0
        default: return 0;
    }
}

// ===========================================================================
// 按需启停（性能优化）
// ===========================================================================
int ii_set_enabled(unsigned long addr, int on) {
    ii_site* s = ii_find(addr);
    if (!s) return -1;
    int want = on ? 1 : 0;
    if (s->enabled == want && !s->pending) return 0;
    s->enabled = want;
    s->pending = 1;
    return 0;
}

int ii_set_cancelable(unsigned long addr, int on) {
    ii_site* s = ii_find(addr);
    if (!s) return -1;
    s->cancelable = on ? 1 : 0;
    return 0;
}

int ii_is_enabled(unsigned long addr) {
    ii_site* s = ii_find(addr);
    return s ? (s->enabled && s->installed) : 0;
}

int ii_pending_count(void) {
    int n = 0;
    for (int i = 0; i < g_nSites; ++i) {
        ii_site* t = site_at(i);
        if (t && t->pending) ++n;
    }
    return n;
}

int ii_set_enabled_all(int on) {
    int n = 0;
    int want = on ? 1 : 0;
    for (int i = 0; i < g_nSites; ++i) {
        ii_site* s = site_at(i);
        if (!s) continue;
        ++n;
        if (s->enabled != want) { s->enabled = want; s->pending = 1; }
    }
    return n;
}

int ii_apply_pending(void) {
    // ★ 不能放栈上：II_MAX_SITES = 262,144，ii_site*[262144] 就是 2 MB。
    //   游戏工作线程栈常只有 1~2 MB，这里会直接爆栈
    //   （ii_uninstall_all 有过同样的 bug，真机上表现为卡死闪退）。
    int total = g_nSites;
    if (total <= 0) return 0;
    if (total > II_MAX_SITES) total = II_MAX_SITES;
    ii_site** list = (ii_site**)malloc((size_t)total * sizeof(ii_site*));
    if (!list) return -9;
    int n = 0;
    for (int i = 0; i < total; ++i) {
        ii_site* t = site_at(i);
        if (t && t->pending) list[n++] = t;
    }

    if (n == 0) { free(list); return 0; }   // 零开销：没有待办就不动线程

    // 只有 16 字节入口模式才需要暂停（写 20 字节非原子）；
    // 常规 4 字节桩点全部原子写，完全不暂停线程。
    int needStop = 0;
    for (int i = 0; i < n; ++i)
        if (list[i]->wideMode) { needStop = 1; break; }

    int stopped = needStop ? suspend_others() : 0;
    if (needStop && stopped < g_stopExpect) {
        // 没停全就不能动 16 字节：线程可能正执行在待改写区域上。
        // 放弃本次全部待办（不清 pending，下次再试）。
        resume_others();
        free(list);
        wd_log("apply_pending 放弃：只暂停 %d/%d 个线程\n", stopped, g_stopExpect);
        return -1;
    }
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        ii_site* s = list[i];
        s->pending = 0;
        if (s->enabled && !s->installed) {
            int sid = site_id(s);
            if (sid >= 0 && site_enable(s, sid) == 0) ++ok;
        } else if (!s->enabled && s->installed) {
            if (site_disable(s) == 0) ++ok;
        }
    }
    if (needStop) resume_others();
    free(list);
    return ok;
}

// ===========================================================================
// 卸载（真删除记录）
// ===========================================================================
// ---------------------------------------------------------------------------
// 退休队列：卸载站点时不能立刻 free(ii_site)
//
// 卸载流程是 site_disable()（4 字节原子写恢复原指令）→ site_release()。
// 但"恢复原指令"只保证后续调用不再跳进 shell；此刻已经跳进 shell 的线程
// 还在 ii_dispatch 里持有 s 指针。如果当场 free(s)，那些线程就会读到
// 已释放内存 —— 真机表现是卸载后几秒随机崩，没有稳定现场。
//
// 所以过期站点先进退休队列，等足够久（且分配次数够多）再真正释放。
// ---------------------------------------------------------------------------
#define II_RETIRE_MAX   32
#define II_RETIRE_DELAY_MS 3000

static struct {
    ii_site* site;
    uint64_t whenMs;
} g_retired[II_RETIRE_MAX];
static int g_nRetired = 0;

static void retire_purge(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    for (int i = 0; i < g_nRetired; ) {
        if (now - g_retired[i].whenMs < II_RETIRE_DELAY_MS) { ++i; continue; }
        free(g_retired[i].site);                       // 真正释放
        g_retired[i] = g_retired[--g_nRetired];
    }
}

static void site_retire(ii_site* s) {
    if (!s) return;
    retire_purge();
    if (g_nRetired >= II_RETIRE_MAX) {
        // 队列满：被迫当场释放（最小化风险，正常不会走到）
        wd_log("[retire] 队列满，直接释放 site=%p\n", (void*)s);
        free(s);
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_retired[g_nRetired].site   = s;
    g_retired[g_nRetired].whenMs = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    g_nRetired++;
}

static void site_release(ii_site* s) {
    if (!s) return;
    if (s->wideMode && s->trampoline) {
        // 重定向的 16 字节模式把 shell+trampoline 放在同一块 8KB 映射里
        // （shell @+0，trampoline @+4096），必须一次释放 8KB；
        // 普通 wideMode 只映射了 4KB。
        size_t msz = s->redirCb ? 8192u : 4096u;
        munmap((void*)s->trampoline, msz);
    }
    if (s->wideMode && s->trampSaved)  { free((void*)s->trampSaved); s->trampSaved = 0; }
    if (s->poolIdx >= 0 && g_reg && s->poolIdx < g_reg->nPool) {
        zh_pool_rec* p = &g_reg->pool[s->poolIdx];
        for (int i = 0; i < p->nSite; ++i)
            if (p->siteAddr[i] == s->addr) { p->siteAddr[i] = 0; break; }
        // 归还 shell 槽位，供后续安装复用
        if (s->shell) zhreg_free_slot(g_reg, s->poolIdx, (uint64_t)s->shell);
    }
    if (g_reg) zhreg_del_site(g_reg, s->addr, g_myId);
    for (int i = 0; i < g_nSites; ++i)
        if (site_at(i) == s) { site_set(i, NULL); break; }
    chain_clear(s);          // 释放回调链
    site_retire(s);          // 延迟释放（见 site_retire 的说明）
}

int ii_uninstall(unsigned long addr) {
    ii_site* s = ii_find(addr);
    if (!s) {
        // 本地没装，可能只是挂在了别的探针的站点上
        if (g_reg) {
            int zidx = zhreg_find_site(g_reg, addr);
            if (zidx >= 0) { zhreg_del_cb(g_reg, zidx, g_myId); return 0; }
        }
        return -1;
    }
    int rc = site_disable(s);   // 内部按需暂停（仅 16 字节模式）
    if (rc != 0) {
        // 原指令没恢复成功，绝不能释放 trampoline/shell：
        // 入口跳转还指着它们，释放后下一次调用就跳进野内存。
        wd_log("uninstall 中止 @0x%lx：site_disable rc=%d（保留站点，可重试）\n", addr, rc);
        return rc;
    }
    site_release(s);
    return 0;
}

void ii_uninstall_all(void) {
    // ★ 不能放栈上：II_MAX_SITES = 256*1024 = 262,144，
    //   unsigned long[262144] 就是 2 MB。游戏工作线程栈常只有 1~2 MB，
    //   ii_engine_destroy() 在真机上就是在这里爆栈崩溃的（t11）。
    //   只按实际站点数分配。
    int total = g_nSites;
    if (total <= 0) return;
    if (total > II_MAX_SITES) total = II_MAX_SITES;

    unsigned long* addrs = (unsigned long*)malloc((size_t)total * sizeof(unsigned long));
    if (!addrs) return;   // 内存不足时宁可不卸载，也不要崩

    wd_log("[uninst_all] total=%d\n", total);
    int n = 0;
    for (int i = 0; i < total; ++i) {
        ii_site* t = site_at(i);
        if (t) addrs[n++] = t->addr;
    }
    wd_log("[uninst_all] 收集到 %d 个站点\n", n);

    // 逐个禁用：4 字节原子写，不暂停线程；16 字节模式内部自行暂停
    for (int i = 0; i < n; ++i) {
        wd_log("[uninst_all] disable #%d addr=0x%lx\n", i, addrs[i]);
        ii_site* s = ii_find(addrs[i]);
        if (s) site_disable(s);
        // 每个站点卸载后强制回到"全部线程运行"的干净状态。
        // 多站点连续卸载时，任何一次暂停未配对都会让后续 dispatch
        // 死在信号处理器里（真机表现为卡死 + ANR）。
        zh_stop_wake_all();
        g_pauseDepth = 0;
        g_stopReq    = 0;
    }
    wd_log("[uninst_all] disable 全部完成\n");

    for (int i = 0; i < n; ++i) {
        wd_log("[uninst_all] release #%d addr=0x%lx\n", i, addrs[i]);
        ii_site* s = ii_find(addrs[i]);
        if (s) site_release(s);
    }
    wd_log("[uninst_all] release 全部完成\n");

    free(addrs);
}

// ===========================================================================
// 引擎实例 API
//
// 这些函数接受显式 ii_engine*，内部逻辑与全局 API 相同，
// 只是把「默认实例」换成传入的实例。
//
// 由于历史代码大量使用全局宏（g_sites 等），这里的实现方式是：
//   1. 保存当前默认实例指针
//   2. 临时切换到目标实例
//   3. 调原有函数
//   4. 恢复
// 这样不改 2,600 行既有逻辑，风险最低。
// ===========================================================================

static ii_engine* g_savedDefault = NULL;
static int        g_switchDepth  = 0;

// 切到实例 e 的上下文（trace / CFI 归属）。
//
// ★ 重构后 g_defaultPtr 恒等于 &g_defaultEngine，不再切换站点表。
//   站点表全局唯一，所有实例共享它；shell 里的 slotId 就是全局表索引。
//   之前切表会让 site_at(slotId) 在错误的表里查，恒返回 NULL，
//   回调永不执行，且销毁顺序一乱就崩。
static ii_engine* engine_enter(ii_engine* e) {
    if (!e) return NULL;
    ii_engine* prev = g_defaultPtr;
    if (g_switchDepth++ == 0) g_savedDefault = prev;
    g_defaultPtr = e;
    return prev;
}

static void engine_leave(void) {
    if (g_switchDepth > 0 && --g_switchDepth == 0) {
        g_defaultPtr = g_savedDefault;
        g_savedDefault = NULL;
    }
}

// ===========================================================================
// 多实例（重构版）
//
// 设计：站点表全局唯一。
//   shell 里硬编码的 slotId 是全局表索引，dispatch 热路径必须能查到它。
//   多实例不再各自持有站点表 —— 那会让 site_at(slotId) 在错误的表里查找，
//   结果恒为 NULL，回调永不执行，且销毁顺序一乱就把进程拖死。
//
//   ii_engine 现在的职责只有两个：
//     1. ownerId  —— 标签，用于"这次安装归属哪个实例"
//     2. ownAddr  —— 记录本实例装过哪些地址，销毁时只撤自己那份
// ===========================================================================

static int engine_own_record(ii_engine* e, unsigned long addr) {
    if (!e || e == &g_defaultEngine) return 0;
    for (int i = 0; i < e->nOwn; ++i)
        if (e->ownAddr[i] == addr) return 0;       // 已有
    if (e->nOwn >= II_ENGINE_MAX_OWN) return -1;
    e->ownAddr[e->nOwn++] = addr;
    return 0;
}

static int engine_own_forget(ii_engine* e, unsigned long addr) {
    if (!e) return -1;
    for (int i = 0; i < e->nOwn; ++i) {
        if (e->ownAddr[i] == addr) {
            e->ownAddr[i] = e->ownAddr[--e->nOwn];
            return 0;
        }
    }
    return -1;
}

int ii_engine_install(ii_engine* e, unsigned long addr, ii_callback cb, void* user) {
    return ii_engine_install_ex(e, addr, cb, user, II_PRIO_DEFAULT);
}

int ii_engine_install_ex(ii_engine* e, unsigned long addr, ii_callback cb,
                         void* user, int priority) {
    if (!e) return -1;
    // 打归属标记：chain_insert 会把 g_curOwner 记到回调节点上，
    // 卸载时据此只摘自己那份（不影响别的实例/全局引擎）。
    int saved = g_curOwner;
    g_curOwner = (e == &g_defaultEngine) ? 0 : e->ownerId;
    // 站点一律进全局表（ii_install_ex 内部用的就是全局表）
    int rc = ii_install_ex(addr, cb, user, priority);
    g_curOwner = saved;
    if (rc != 0) return rc;
    if (engine_own_record(e, addr) != 0) {
        // 记账满了：撤回刚装的回调，避免"装了但记不住"
        ii_remove_callback(addr, cb, user);
        return -20;
    }
    e->nSites++;
    return 0;
}

int ii_engine_uninstall(ii_engine* e, unsigned long addr) {
    if (!e) return -1;

    // ★ 只摘本实例的回调，不能直接 ii_uninstall(addr)。
    //   ii_uninstall 是"撤整个站点"：会把别的实例 / 全局引擎挂在
    //   同一地址上的回调一起消掉，还会把指令恢复成原始状态 ——
    //   那对"我只是想注销我这一份"的语义是破坏性的。
    //   t11 真机现象：destroy(A) 之后 B 和全局引擎的回调也全停了。
    ii_site* s = ii_find(addr);
    if (!s) { engine_own_forget(e, addr); return -1; }

    // 按 owner 精确摘除：只撤本实例挂上去的回调。
    //
    // ★ 必须先把 next 取出来再删：chain_remove 内部 free(n)，
    //   删完再读 n->next 就是 use-after-free（真机上表现为
    //   卸载后随机崩，且崩在别的线程里，没有稳定现场）。
    int want = (e == &g_defaultEngine) ? 0 : e->ownerId;
    int removed = 0;
    ii_cb_node* n = s->cbs;
    while (n) {
        ii_cb_node* nx = n->next;          // 先缓存
        if (n->alive && n->owner == want) {
            if (chain_remove(s, n->cb, n->user) == 0) ++removed;
        }
        n = nx;
    }
    if (want == 0 && s->cb) { s->cb = NULL; s->user = NULL; ++removed; }

    if (removed > 0) {
        engine_own_forget(e, addr);
        if (e->nSites > 0) e->nSites--;
    }

    // 站点上已经没有「活着的」回调了 → 才真正卸载站点（恢复原指令）。
    // 注意不能只看 !s->cbs：chain_remove 现在是懒删除，死节点仍留在链上，
    // 只把 alive 清 0。
    int hasLive = 0;
    for (ii_cb_node* k = s->cbs; k; k = k->next)
        if (k->alive && k->cb) { hasLive = 1; break; }
    if (!hasLive && !s->cb) {
        ii_uninstall(addr);
        return 0;
    }
    return removed > 0 ? 0 : -1;
}

void ii_engine_uninstall_all(ii_engine* e) {
    if (!e) return;
    if (e == &g_defaultEngine) {
        ii_uninstall_all();           // 全局：全撤
        return;
    }
    // 非默认实例：逐个地址按 owner 精确摘除。
    //
    // ★ 必须走 ii_engine_uninstall（它会按 owner 过滤、且只在站点空了
    //   才真正卸载），不能直接 ii_uninstall —— 后者是"撤整个站点"，
    //   会把别的实例 / 全局引擎挂在同一地址上的回调一起消掉。
    //   t11 真机现象就是 destroy(A) 之后 B 和全局引擎的回调全停了。
    while (e->nOwn > 0) {
        unsigned long a = e->ownAddr[e->nOwn - 1];
        e->nOwn--;                    // 先出栈，ii_engine_uninstall 里还会 forget
        ii_engine_uninstall(e, a);
    }
    e->nSites = 0;
    e->nOwn   = 0;
}

int ii_engine_count(ii_engine* e) {
    return e ? e->nSites : 0;
}

ii_site* ii_engine_find(ii_engine* e, unsigned long addr) {
    if (!e) return NULL;
    // 站点在全局表里
    return ii_find(addr);
}


int ii_engine_trace_begin(ii_engine* e) {
    if (!e) return -1;
    e->traceOn = 1;
    return 0;
}

int ii_engine_trace_end(ii_engine* e) {
    if (!e) return -1;
    e->traceOn = 0;
    return 0;
}

int ii_engine_trace_read(ii_engine* e, int idx, ii_trace_rec* out) {
    if (!e || !out || idx < 0 || idx >= II_TRACE_N) return -1;
    uint32_t seq = __atomic_load_n(&e->traceSeq, __ATOMIC_ACQUIRE);
    if ((uint32_t)idx >= seq) return -2;
    uint32_t want = seq - 1 - (uint32_t)idx;
    *out = e->trace[want % II_TRACE_N];
    return 0;
}

int ii_engine_trace_dump(ii_engine* e) {
    if (!e) return -1;
    engine_enter(e);
    ii_trace_dump();
    engine_leave();
    return 0;
}

int ii_engine_frame_cfi_load(ii_engine* e, const char* path) {
    if (!e) return -1;
    engine_enter(e);
    int rc = ii_frame_cfi_load(path);
    engine_leave();
    return rc;
}

int ii_engine_frame_cfi_loaded(ii_engine* e) {
    return e ? (int)e->cfiCount : 0;
}

int ii_engine_get_stats(ii_engine* e, ii_engine_stats* out) {
    if (!e || !out) return -1;
    out->nSites         = e->nSites;
    out->capacity       = II_MAX_SITES;
    out->traceSeq       = e->traceSeq;
    out->traceOn        = e->traceOn;
    out->poolDeaths     = e->poolDeaths;
    out->poolRecoveries = e->poolRecoveries;
    out->cfiLoaded      = (int)e->cfiCount;
    return 0;
}

// ===========================================================================
// 仅供测试：假站点注册
//
// 真实安装需要可插桩的内存地址；host 测试没有游戏 so，
// 所以这里绕过插桩，只建站点记录 + 回调链，供 ii_dispatch 测试用。
// ===========================================================================

int ii_test_register_site(unsigned long fakeAddr, ii_callback cb, void* user, int priority) {
    if (!cb) return -1;
    ii_site* s = (ii_site*)calloc(1, sizeof *s);
    if (!s) return -9;
    s->addr    = fakeAddr;
    s->enabled = 1;
    s->regIdx  = -1;
    s->retKind = -1;
    s->priority = priority;
    if (chain_insert(s, cb, user, priority) != 0) { free(s); return -9; }
    int sid = site_append(s);
    if (sid < 0) { chain_clear(s); free(s); return -3; }
    s->installed = 1;
    return sid;
}

void ii_test_clear_sites(void) {
    for (int i = g_nSites - 1; i >= 0; --i) {
        ii_site* s = site_at(i);
        if (!s) continue;
        site_set(i, NULL);
        chain_clear(s);
        free(s);
    }
    g_nSites = 0;
}

int ii_test_add_cb(unsigned long fakeAddr, ii_callback cb, void* user, int priority) {
    ii_site* s = ii_find(fakeAddr);
    if (!s) return -2;
    return chain_insert(s, cb, user, priority);
}

int ii_test_kill_cb(unsigned long fakeAddr, ii_callback cb) {
    ii_site* s = ii_find(fakeAddr);
    if (!s) return -2;
    for (ii_cb_node* n = s->cbs; n; n = n->next)
        if (n->cb == cb) { n->alive = 0; return 0; }
    return -1;
}
