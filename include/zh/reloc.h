// ============================================================================
// arm64 相对跳转重定位 + 远程桩
//
// 三个必须解决的问题：
//   1) PC 变了：立即数相对 PC，搬到 trampoline 后要按新 PC 重算
//   2) 目标被覆盖：原函数入口 16 字节已被 detour 覆盖，
//      目标若落在这 16 字节内，必须改指向 trampoline 的对应位置
//   3) 距离太远：cbz/b.cond 只有 ±1MB、tbz 只有 ±32KB，
//      而游戏 .so 附近 ±1MB 完全没有可执行空隙 —— 必须用远程桩
//
// 远程桩（4 条指令 = 16 字节）：
//     ldr x16, #8      ; 从 PC+8 加载目标地址
//     br  x16          ; 跳过去
//     .quad target     ; 目标地址（8 字节，位于 PC+8）
// ============================================================================
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 是否可以重定位（b/bl、b.cond、cbz/cbnz、tbz/tbnz）
int zh_needs_reloc(uint32_t insn);

// 该指令的跳转距离极限（字节）。非跳转指令返回 0
int64_t zh_reach(uint32_t insn);

// 取出跳转指令的目标地址（已应用覆盖区映射）
// 返回 1 成功；0 表示不是跳转指令
int zh_get_target(uint32_t insn, uint64_t old_pc,
                  uint64_t cov_start, uint64_t cov_end, uint64_t tramp_base,
                  uint64_t* out_target);

// 用 new_pc → target 重写指令
// 返回 0 表示超出该指令的立即数范围（调用方应改用远程桩）
uint32_t zh_set_target(uint32_t insn, uint64_t new_pc, uint64_t target);

// 远程桩指令（4 条，写入 16 字节）
#define ZH_STUB_WORDS 4
void zh_make_stub(uint32_t* dst, uint64_t target);

// 兼容旧接口
uint32_t zh_relocate2(uint32_t insn, uint64_t old_pc, uint64_t new_pc,
                      uint64_t cov_start, uint64_t cov_end,
                      uint64_t tramp_base, int* need_reloc);
uint32_t zh_relocate(uint32_t insn, uint64_t old_pc, uint64_t new_pc,
                     int* need_reloc);

#ifdef __cplusplus
}
#endif
