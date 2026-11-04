// 自动生成 by offline_pools.py —— 离线分析出的 trampoline 池位置
// 数据来源：game.so 代码段的连续 0x00/NOP 填充区
// 用途：替代运行时扫描（扫描 275MB 要 60 秒，会导致 ANR）
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t vaddr;   // 相对基址
    uint32_t size;    // 字节数
} zh_poolhint;

static const zh_poolhint g_poolhint[] = {
    {0x1334b08c, 315660},
    {0x136e08fc, 250724},
    {0x13763d00, 21248},
    {0x387a304, 8196},
    {0x3856e94, 7236},
};
#define N_POOLHINT 5

