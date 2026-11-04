// 指令分类测试：验证「哪些指令不能插桩」的判断
#include <stdio.h>
#include <stdint.h>

// 复刻 instr_hook.c 里的判断逻辑（独立实现，用于交叉验证）
static int is_ret(uint32_t w)  { return (w & 0xFFFFFC1Fu) == 0xD65F0000u; }
static int is_b(uint32_t w)    { return (w & 0xFC000000u) == 0x14000000u; }
static int is_bcond(uint32_t w){ return (w & 0xFF000010u) == 0x54000000u; }
static int is_cbz(uint32_t w)  { return (w & 0x7E000000u) == 0x34000000u; }
static int is_tbz(uint32_t w)  { return (w & 0x7E000000u) == 0x36000000u; }
static int is_ldr_lit(uint32_t w){ return (w & 0x3B000000u) == 0x18000000u; }
static int is_adrp(uint32_t w) { return (w & 0x1F000000u) == 0x10000000u; }

static int fails = 0;

static void expect(const char* name, uint32_t w, int want) {
    int got = is_ret(w)||is_b(w)||is_bcond(w)||is_cbz(w)||is_tbz(w)||is_ldr_lit(w)||is_adrp(w);
    if (!!got != !!want) {
        printf("FAIL: %-14s 0x%08x 期望%s，实际%s\n", name, w,
               want?"拒绝":"接受", got?"拒绝":"接受");
        fails++;
    } else {
        printf("  OK: %-14s 0x%08x → %s\n", name, w, want?"拒绝":"接受");
    }
}

int main(void) {
    printf("=== 指令分类 ===\n");
    expect("ret",        0xD65F03C0u, 1);
    expect("b",          0x14000000u, 1);
    expect("b.cond",     0x54000000u, 1);
    expect("cbz",        0x34000000u, 1);
    expect("tbz",        0x36000000u, 1);
    expect("ldr-literal",0x58000050u, 1);
    expect("adrp",       0x90000000u, 1);
    // 这些应该可以插桩
    expect("mov x20,x0", 0xAA0003F4u, 0);
    expect("nop",        0xD503201Fu, 0);
    expect("add x0,x0,1",0x91000400u, 0);
    printf(fails ? "test_insn: %d 项失败\n" : "test_insn: 全部通过\n", fails);
    return fails ? 1 : 0;
}
