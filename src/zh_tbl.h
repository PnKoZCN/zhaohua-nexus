// 表访问抽象层（内部头文件）
#pragma once
#include <stdint.h>
#include "zh/zh_db.h"

int zh_tbl_attach(zh_db* db);
int zh_tbl_attached(void);
int zh_tbl_count(int type);

int zh_tbl_rettype(int i, const char** name, uint32_t* off,
                   int* kind, const char** typeName);
int zh_tbl_blcall(int i, const char** func, uint32_t* callOff,
                  const char** target, uint32_t* targetOff);
int zh_tbl_fieldacc(int i, const char** func, uint16_t* off, int* base,
                    int* size, int* wr);
int zh_tbl_frame(int i, const char** func, uint32_t* frameSize,
                 uint32_t* slotStart, int* slotCount);
int zh_tbl_slot(int i, uint16_t* spOff, int* size, uint32_t* insnOff);
int zh_tbl_const(int i, int* funcIdx, int* kind, int* width,
                 uint32_t* insnOff, uint32_t* value);
int zh_tbl_retpoint(int i, int* funcIdx, int* isLast, uint32_t* retOff);
const char* zh_tbl_constfunc(int i);
const char* zh_tbl_retfunc(int i);
int zh_tbl_poolhint(int i, uint32_t* vaddr, uint32_t* size);

int zh_tbl_rettype_by_off(uint32_t off, int* kind, const char** typeName);
int zh_tbl_rettype_by_name(const char* name, int* kind, const char** typeName);
int zh_tbl_frame_by_name(const char* name, uint32_t* frameSize,
                         uint32_t* slotStart, int* slotCount);
int zh_tbl_constfunc_index(const char* name);
int zh_tbl_retfunc_index(const char* name);
