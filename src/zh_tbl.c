// ===========================================================================
// 表访问抽象层
//
// 目标：引擎内部不再直接引用 g_xxx[] 数组，而是通过这里访问。
//       这样「编译期表」和「运行期数据文件」可以任选其一。
//
// 策略：
//   · zh_tbl_attach(db) 后，优先用运行期表
//   · 未 attach 时，回退到编译期表（保证老行为不变）
//   · 两种表格式不同，这里做统一封装
// ===========================================================================
#include "zh/zh_db.h"
#include "zh/instr_hook.h"
#include <string.h>
#include <stddef.h>

// 编译期表（fallback）
#include "rettype_db.h"
#include "blcall_db.h"
#include "fields_real.h"
#include "frame_db.h"
#include "const_db.h"
#include "ret_db.h"
#include "poolhint_db.h"

static zh_db  g_db;
static int    g_hasDb = 0;

int zh_tbl_attach(zh_db* db) {
    if (!db) { g_hasDb = 0; return -1; }
    g_db = *db;
    g_hasDb = 1;
    return 0;
}

int zh_tbl_attached(void) { return g_hasDb; }

// ---- 统一的条目数查询 ----------------------------------------------------
int zh_tbl_count(int type) {
    if (g_hasDb) {
        const void* d; uint32_t c, e;
        return zh_db_table(&g_db, (uint32_t)type, &d, &c, &e);
    }
    switch (type) {
        case ZH_TBL_RETTYPE:   return N_RETTYPE;
        case ZH_TBL_BLCALL:    return N_BLCALL;
        case ZH_TBL_CONST:     return N_CONST;
        case ZH_TBL_FRAME:     return N_FRAME;
        case ZH_TBL_SLOT:      return N_SLOT;
        case ZH_TBL_FIELDACC:  return N_FIELDACC;
        case ZH_TBL_CONSTFUNC: return N_CONSTFUNC;
        case ZH_TBL_POOLHINT:  return N_POOLHINT;
        case ZH_TBL_RETFUNC:   return N_RETFUNC;
        case ZH_TBL_RETPOINT:  return N_RETPOINT;
        default: return 0;
    }
}

// ---- 字符串统一访问 ------------------------------------------------------
// 编译期表：直接是 const char*
// 运行期表：条目里存的是池内偏移
// ---- 各表的统一读取接口 --------------------------------------------------
//
// 每条记录转换成「统一视图」，调用方不用管底层格式。

int zh_tbl_rettype(int i, const char** name, uint32_t* off,
                   int* kind, const char** typeName) {
    if (i < 0 || i >= zh_tbl_count(ZH_TBL_RETTYPE)) return -1;
    if (g_hasDb) {
        const uint8_t* d = (const uint8_t*)g_db.tbl[ZH_TBL_RETTYPE].data + (size_t)i * 16;
        uint32_t nameOff = *(const uint32_t*)(d + 0);
        uint32_t o       = *(const uint32_t*)(d + 4);
        uint32_t k       = *(const uint32_t*)(d + 8);
        uint32_t tnOff   = *(const uint32_t*)(d + 12);
        if (name)     *name     = nameOff ? zh_db_str(&g_db, nameOff) : NULL;
        if (off)      *off      = o;
        if (kind)     *kind     = (int)k;
        if (typeName) *typeName = tnOff ? zh_db_str(&g_db, tnOff) : NULL;
    } else {
        if (name)     *name     = g_rettype[i].name;
        if (off)      *off      = g_rettype[i].off;
        if (kind)     *kind     = (int)g_rettype[i].kind;
        if (typeName) *typeName = g_rettype[i].typeName;
    }
    return 0;
}

int zh_tbl_blcall(int i, const char** func, uint32_t* callOff,
                  const char** target, uint32_t* targetOff) {
    if (i < 0 || i >= zh_tbl_count(ZH_TBL_BLCALL)) return -1;
    if (g_hasDb) {
        const uint8_t* d = (const uint8_t*)g_db.tbl[ZH_TBL_BLCALL].data + (size_t)i * 16;
        uint32_t f = *(const uint32_t*)(d + 0);
        uint32_t co= *(const uint32_t*)(d + 4);
        uint32_t t = *(const uint32_t*)(d + 8);
        uint32_t to= *(const uint32_t*)(d + 12);
        if (func)      *func      = zh_db_str(&g_db, f);
        if (callOff)   *callOff   = co;
        if (target)    *target    = zh_db_str(&g_db, t);
        if (targetOff) *targetOff = to;
    } else {
        if (func)      *func      = g_blcall[i].func;
        if (callOff)   *callOff   = g_blcall[i].callOff;
        if (target)    *target    = g_blcall[i].target;
        if (targetOff) *targetOff = g_blcall[i].targetOff;
    }
    return 0;
}

int zh_tbl_fieldacc(int i, const char** func, uint16_t* off, int* base,
                    int* size, int* wr) {
    if (i < 0 || i >= zh_tbl_count(ZH_TBL_FIELDACC)) return -1;
    if (g_hasDb) {
        const uint8_t* d = (const uint8_t*)g_db.tbl[ZH_TBL_FIELDACC].data + (size_t)i * 12;
        uint32_t f = *(const uint32_t*)(d + 0);
        if (func) *func = zh_db_str(&g_db, f);
        if (off)  *off  = *(const uint16_t*)(d + 4);
        if (base) *base = (int)d[6];
        if (size) *size = (int)d[7];
        if (wr)   *wr   = (int)d[8];
    } else {
        if (func) *func = g_fieldacc[i].func;
        if (off)  *off  = g_fieldacc[i].off;
        if (base) *base = (int)g_fieldacc[i].base;
        if (size) *size = (int)g_fieldacc[i].size;
        if (wr)   *wr   = (int)g_fieldacc[i].wr;
    }
    return 0;
}

int zh_tbl_frame(int i, const char** func, uint32_t* frameSize,
                 uint32_t* slotStart, int* slotCount) {
    if (i < 0 || i >= zh_tbl_count(ZH_TBL_FRAME)) return -1;
    if (g_hasDb) {
        const uint8_t* d = (const uint8_t*)g_db.tbl[ZH_TBL_FRAME].data + (size_t)i * 12;
        uint32_t f = *(const uint32_t*)(d + 0);
        if (func)      *func      = zh_db_str(&g_db, f);
        if (frameSize) *frameSize = *(const uint32_t*)(d + 4);
        if (slotStart) *slotStart = *(const uint32_t*)(d + 8);
        if (slotCount) *slotCount = (int)*(const uint16_t*)(d + 10);
    } else {
        if (func)      *func      = g_frame[i].func;
        if (frameSize) *frameSize = g_frame[i].frameSize;
        if (slotStart) *slotStart = g_frame[i].slotStart;
        if (slotCount) *slotCount = (int)g_frame[i].slotCount;
    }
    return 0;
}

int zh_tbl_slot(int i, uint16_t* spOff, int* size, uint32_t* insnOff) {
    if (i < 0 || i >= zh_tbl_count(ZH_TBL_SLOT)) return -1;
    if (g_hasDb) {
        const uint8_t* d = (const uint8_t*)g_db.tbl[ZH_TBL_SLOT].data + (size_t)i * 8;
        if (spOff)   *spOff   = *(const uint16_t*)(d + 0);
        if (size)    *size    = (int)d[2];
        if (insnOff) *insnOff = *(const uint32_t*)(d + 4);
    } else {
        if (spOff)   *spOff   = g_slot[i].spOff;
        if (size)    *size    = (int)g_slot[i].size;
        if (insnOff) *insnOff = g_slot[i].insnOff;
    }
    return 0;
}

int zh_tbl_const(int i, int* funcIdx, int* kind, int* width,
                 uint32_t* insnOff, uint32_t* value) {
    if (i < 0 || i >= zh_tbl_count(ZH_TBL_CONST)) return -1;
    if (g_hasDb) {
        const uint8_t* d = (const uint8_t*)g_db.tbl[ZH_TBL_CONST].data + (size_t)i * 12;
        if (funcIdx) *funcIdx = (int)*(const uint16_t*)(d + 0);
        if (kind)    *kind    = (int)d[2];
        if (width)   *width   = (int)d[3];
        if (insnOff) *insnOff = *(const uint32_t*)(d + 4);
        if (value)   *value   = *(const uint32_t*)(d + 8);
    } else {
        if (funcIdx) *funcIdx = (int)g_const[i].funcIdx;
        if (kind)    *kind    = (int)g_const[i].kind;
        if (width)   *width   = (int)g_const[i].width;
        if (insnOff) *insnOff = g_const[i].insnOff;
        if (value)   *value   = g_const[i].value;
    }
    return 0;
}

int zh_tbl_retpoint(int i, int* funcIdx, int* isLast, uint32_t* retOff) {
    if (i < 0 || i >= zh_tbl_count(ZH_TBL_RETPOINT)) return -1;
    if (g_hasDb) {
        const uint8_t* d = (const uint8_t*)g_db.tbl[ZH_TBL_RETPOINT].data + (size_t)i * 8;
        if (funcIdx) *funcIdx = (int)*(const uint16_t*)(d + 0);
        if (isLast)  *isLast  = (int)d[2];
        if (retOff)  *retOff  = *(const uint32_t*)(d + 4);
    } else {
        if (funcIdx) *funcIdx = (int)g_retpoint[i].funcIdx;
        if (isLast)  *isLast  = (int)g_retpoint[i].isLast;
        if (retOff)  *retOff  = g_retpoint[i].retOff;
    }
    return 0;
}

const char* zh_tbl_constfunc(int i) {
    if (i < 0 || i >= zh_tbl_count(ZH_TBL_CONSTFUNC)) return NULL;
    if (g_hasDb) {
        uint32_t off = *(const uint32_t*)((const uint8_t*)g_db.tbl[ZH_TBL_CONSTFUNC].data + (size_t)i * 4);
        return zh_db_str(&g_db, off);
    }
    return g_constfunc[i];
}

const char* zh_tbl_retfunc(int i) {
    if (i < 0 || i >= zh_tbl_count(ZH_TBL_RETFUNC)) return NULL;
    if (g_hasDb) {
        uint32_t off = *(const uint32_t*)((const uint8_t*)g_db.tbl[ZH_TBL_RETFUNC].data + (size_t)i * 4);
        return zh_db_str(&g_db, off);
    }
    return g_retfunc[i];
}

int zh_tbl_poolhint(int i, uint32_t* vaddr, uint32_t* size) {
    if (i < 0 || i >= zh_tbl_count(ZH_TBL_POOLHINT)) return -1;
    if (g_hasDb) {
        const uint8_t* d = (const uint8_t*)g_db.tbl[ZH_TBL_POOLHINT].data + (size_t)i * 8;
        if (vaddr) *vaddr = *(const uint32_t*)(d + 0);
        if (size)  *size  = *(const uint32_t*)(d + 4);
    } else {
        if (vaddr) *vaddr = g_poolhint[i].vaddr;
        if (size)  *size  = g_poolhint[i].size;
    }
    return 0;
}

// 指针形式的返回（给内部代码用）
int zh_tbl_rettype_by_off(uint32_t off, int* kind, const char** typeName) {
    int n = zh_tbl_count(ZH_TBL_RETTYPE);
    for (int i = 0; i < n; ++i) {
        uint32_t o; int k; const char* tn;
        zh_tbl_rettype(i, NULL, &o, &k, &tn);
        if (o == off) {
            if (kind) *kind = k;
            if (typeName) *typeName = tn;
            return 0;
        }
    }
    return -1;
}

int zh_tbl_rettype_by_name(const char* name, int* kind, const char** typeName) {
    if (!name) return -1;
    int n = zh_tbl_count(ZH_TBL_RETTYPE);
    for (int i = 0; i < n; ++i) {
        const char* fn; uint32_t o; int k; const char* tn;
        zh_tbl_rettype(i, &fn, &o, &k, &tn);
        if (fn && !strcmp(fn, name)) {
            if (kind) *kind = k;
            if (typeName) *typeName = tn;
            return 0;
        }
    }
    return -1;
}

int zh_tbl_frame_by_name(const char* name, uint32_t* frameSize,
                         uint32_t* slotStart, int* slotCount) {
    if (!name) return -1;
    int n = zh_tbl_count(ZH_TBL_FRAME);
    for (int i = 0; i < n; ++i) {
        const char* fn;
        zh_tbl_frame(i, &fn, NULL, NULL, NULL);
        if (fn && !strcmp(fn, name)) {
            return zh_tbl_frame(i, NULL, frameSize, slotStart, slotCount);
        }
    }
    return -1;
}

int zh_tbl_constfunc_index(const char* name) {
    if (!name) return -1;
    int n = zh_tbl_count(ZH_TBL_CONSTFUNC);
    for (int i = 0; i < n; ++i) {
        const char* f = zh_tbl_constfunc(i);
        if (f && !strcmp(f, name)) return i;
    }
    return -1;
}

int zh_tbl_retfunc_index(const char* name) {
    if (!name) return -1;
    int n = zh_tbl_count(ZH_TBL_RETFUNC);
    for (int i = 0; i < n; ++i) {
        const char* f = zh_tbl_retfunc(i);
        if (f && !strcmp(f, name)) return i;
    }
    return -1;
}
