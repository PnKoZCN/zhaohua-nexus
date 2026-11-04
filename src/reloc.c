#include "zh/reloc.h"

static int64_t sext(uint64_t v, int bits) {
    uint64_t sign = 1ULL << (bits - 1);
    return (int64_t)((v ^ sign) - sign);
}

int zh_needs_reloc(uint32_t insn) {
    if ((insn & 0x1F000000) == 0x10000000) return 1;   // adr/adrp
    if ((insn & 0x3B000000) == 0x18000000) return 1;   // ldr-literal
    if ((insn & 0x7C000000) == 0x14000000) return 1;   // b/bl
    if ((insn & 0xFF000010) == 0x54000000) return 1;   // b.cond
    if ((insn & 0x7E000000) == 0x34000000) return 1;   // cbz/cbnz
    if ((insn & 0x7E000000) == 0x36000000) return 1;   // tbz/tbnz
    return 0;
}

int64_t zh_reach(uint32_t insn) {
    if ((insn & 0x1F000000) == 0x10000000) return 1LL << 32;  // adrp   ±4GB
    if ((insn & 0x3B000000) == 0x18000000) return 1LL << 20;  // ldr-lit ±1MB
    if ((insn & 0x7C000000) == 0x14000000) return 1LL << 27;  // b/bl  ±128MB
    if ((insn & 0xFF000010) == 0x54000000) return 1LL << 20;  // b.cond ±1MB
    if ((insn & 0x7E000000) == 0x34000000) return 1LL << 20;  // cbz    ±1MB
    if ((insn & 0x7E000000) == 0x36000000) return 1LL << 15;  // tbz    ±32KB
    return 0;
}

static uint64_t map_target(uint64_t target,
                           uint64_t cov_start, uint64_t cov_end,
                           uint64_t tramp_base) {
    if (cov_end > cov_start && target >= cov_start && target < cov_end)
        return tramp_base + (target - cov_start);
    return target;
}

int zh_get_target(uint32_t insn, uint64_t old_pc,
                  uint64_t cov_start, uint64_t cov_end, uint64_t tramp_base,
                  uint64_t* out) {
    uint64_t t;
    if ((insn & 0x1F000000) == 0x10000000) {          // adr/adrp
        uint32_t immlo = (insn >> 29) & 3u;
        uint32_t immhi = (insn >> 5) & 0x7FFFFu;
        int64_t imm = (int64_t)((immhi << 2) | immlo);   // immhi:immlo 拼接
        if (imm & (1LL << 20)) imm -= (1LL << 21);       // 符号扩展 21 位
        t = (old_pc & ~0xFFFULL) + (uint64_t)(imm << 12);
        *out = t;                                      // adrp 页地址映射到自身
        return 1;
    }
    if ((insn & 0x3B000000) == 0x18000000) {          // ldr-literal
        t = old_pc + sext((insn >> 5) & 0x7FFFFu, 19) * 4;
        *out = map_target(t, cov_start, cov_end, tramp_base);
        return 1;
    }
    if ((insn & 0x7C000000) == 0x14000000) {          // b/bl
        t = old_pc + sext(insn & 0x3FFFFFFu, 26) * 4;
    } else if ((insn & 0xFF000010) == 0x54000000) {   // b.cond
        t = old_pc + sext((insn >> 5) & 0x7FFFFu, 19) * 4;
    } else if ((insn & 0x7E000000) == 0x34000000) {   // cbz/cbnz
        t = old_pc + sext((insn >> 5) & 0x7FFFFu, 19) * 4;
    } else if ((insn & 0x7E000000) == 0x36000000) {   // tbz/tbnz
        t = old_pc + sext((insn >> 5) & 0x3FFFu, 14) * 4;
    } else {
        return 0;
    }
    *out = map_target(t, cov_start, cov_end, tramp_base);
    return 1;
}

uint32_t zh_set_target(uint32_t insn, uint64_t new_pc, uint64_t target) {
    int64_t noff = (int64_t)target - (int64_t)new_pc;

    if ((insn & 0x1F000000) == 0x10000000) {          // adr/adrp
        int64_t pageOff = (int64_t)(target & ~0xFFFULL) -
                          (int64_t)(new_pc & ~0xFFFULL);
        if (pageOff & 0xFFF) return 0;
        int64_t pages = pageOff >> 12;
        if (pages < -(1LL << 20) || pages >= (1LL << 20)) return 0;
        uint32_t immlo = (uint32_t)(pages & 3);
        uint32_t immhi = (uint32_t)((pages >> 2) & 0x7FFFF);
        return (insn & 0x9F00001Fu) | (immlo << 29) | (immhi << 5);
    }
    if ((insn & 0x3B000000) == 0x18000000) {          // ldr-literal
        if (noff < -(1LL<<20) || noff >= (1LL<<20)) return 0;
        if (noff & 3) return 0;
        return (insn & 0xFF00001Fu) | (((uint32_t)(noff >> 2) & 0x7FFFFu) << 5);
    }

    if ((insn & 0x7C000000) == 0x14000000) {          // b/bl
        if (noff < -(1LL<<27) || noff >= (1LL<<27)) return 0;
        if (noff & 3) return 0;
        return (insn & 0xFC000000u) | ((uint32_t)((noff >> 2) & 0x3FFFFFFu));
    }
    if ((insn & 0xFF000010) == 0x54000000) {          // b.cond
        if (noff < -(1LL<<20) || noff >= (1LL<<20)) return 0;
        if (noff & 3) return 0;
        return (insn & 0xFF00001Fu) | (((uint32_t)(noff >> 2) & 0x7FFFFu) << 5);
    }
    if ((insn & 0x7E000000) == 0x34000000) {          // cbz/cbnz
        if (noff < -(1LL<<20) || noff >= (1LL<<20)) return 0;
        if (noff & 3) return 0;
        return (insn & 0xFF00001Fu) | (((uint32_t)(noff >> 2) & 0x7FFFFu) << 5);
    }
    if ((insn & 0x7E000000) == 0x36000000) {          // tbz/tbnz
        if (noff < -(1LL<<15) || noff >= (1LL<<15)) return 0;
        if (noff & 3) return 0;
        return (insn & 0xFFF8001Fu) | (((uint32_t)(noff >> 2) & 0x3FFFu) << 5);
    }
    return insn;
}

void zh_make_stub(uint32_t* dst, uint64_t target) {
    dst[0] = 0x58000050u;                 // ldr x16, #8
    dst[1] = 0xD61F0200u;                 // br  x16
    dst[2] = (uint32_t)(target & 0xFFFFFFFFu);
    dst[3] = (uint32_t)((target >> 32) & 0xFFFFFFFFu);
}

// ---- 兼容旧接口 ----
uint32_t zh_relocate2(uint32_t insn, uint64_t old_pc, uint64_t new_pc,
                      uint64_t cov_start, uint64_t cov_end,
                      uint64_t tramp_base, int* need_reloc) {
    if (need_reloc) *need_reloc = 0;
    if (!zh_needs_reloc(insn)) return insn;
    if (need_reloc) *need_reloc = 1;
    uint64_t t;
    if (!zh_get_target(insn, old_pc, cov_start, cov_end, tramp_base, &t)) return 0;
    return zh_set_target(insn, new_pc, t);
}

uint32_t zh_relocate(uint32_t insn, uint64_t old_pc, uint64_t new_pc,
                     int* need_reloc) {
    return zh_relocate2(insn, old_pc, new_pc, 0, 0, 0, need_reloc);
}
