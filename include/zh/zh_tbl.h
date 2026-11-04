// ===========================================================================
// 数据表查询接口
//
// 引擎有两条数据源通路：
//   1. 编译期静态表（const_db.h / ret_db.h / frame_db.h / fields_real.h /
//      poolhint_db.h）—— g_hasDb == 0 时使用
//   2. 运行期 mmap 的 zh_db —— 调 zh_tbl_attach() 之后 g_hasDb == 1
//
// 之前的缺陷：这 18 个函数只在 src/zh_tbl.c 里定义，没有任何头文件声明，
//   导致外部代码无法 attach 运行期数据表，"运行期数据表"能力实际不可用。
//   本头文件补齐声明。用法：
//
//       zh_db db;
//       if (zh_db_load(&db, "/path/zh_db_full.bin") == ZH_DB_OK)
//           zh_tbl_attach(&db);
//       // 之后所有 ii_* 查询自动走 mmap 数据
// ===========================================================================
#pragma once
#include <stdint.h>
#include "zh_db.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- 数据源切换 ----------------------------------------------------------
// 挂接运行期数据表。传 NULL 表示退回编译期静态表。
int  zh_tbl_attach(zh_db* db);
// 当前是否已挂接运行期数据表（1=是，0=用静态表）
int  zh_tbl_attached(void);

// ---- 条目数 --------------------------------------------------------------
// type 取 ZH_TBL_* 之一；未知类型返回 0
int  zh_tbl_count(int type);

// ---- 逐条读取（返回 0 成功，非 0 表示 i 越界或该表不存在）----------------

// 返回类型表：名称、首写指令偏移、返回种类、类型名
int  zh_tbl_rettype(int i, const char** name, uint32_t* off,
                    int* kind, const char** typeName);

// 方法调用表：调用者、调用点偏移、被调方法、被调偏移
int  zh_tbl_blcall(int i, const char** func, uint32_t* callOff,
                   const char** target, uint32_t* targetOff);

// 字段访问表：函数名、字段偏移、基址寄存器、宽度、读写
int  zh_tbl_fieldacc(int i, const char** func, uint16_t* off,
                     int* base, int* size, int* wr);

// 栈帧表：函数名、帧大小、首槽偏移、槽数
int  zh_tbl_frame(int i, const char** func, uint32_t* frameSize,
                  uint32_t* slotStart, int* slotCount);

// 栈槽表：sp 偏移、宽度、首写指令偏移
int  zh_tbl_slot(int i, uint16_t* spOff, int* size, uint32_t* insnOff);

// 常量表：所属函数索引、种类、宽度、指令偏移、值
int  zh_tbl_const(int i, int* funcIdx, int* kind, int* width,
                  uint32_t* insnOff, uint32_t* value);

// 返回点表：所属函数索引、是否最后一条、返回点偏移
int  zh_tbl_retpoint(int i, int* funcIdx, int* isLast, uint32_t* retOff);

// 常量所属函数名（i 是 constfunc 表下标）
const char* zh_tbl_constfunc(int i);
// 有返回点的函数名（i 是 retfunc 表下标）
const char* zh_tbl_retfunc(int i);

// 字符串池提示：虚拟地址、大小
int  zh_tbl_poolhint(int i, uint32_t* vaddr, uint32_t* size);

// ---- 反查 ----------------------------------------------------------------

// 按指令偏移反查返回类型
int  zh_tbl_rettype_by_off(uint32_t off, int* kind, const char** typeName);
// 按函数名查返回类型
int  zh_tbl_rettype_by_name(const char* name, int* kind, const char** typeName);
// 按函数名查栈帧信息
int  zh_tbl_frame_by_name(const char* name, uint32_t* frameSize,
                          uint32_t* slotStart, int* slotCount);
// 按函数名查 constfunc 表下标（找不到返回 -1）
int  zh_tbl_constfunc_index(const char* name);
// 按函数名查 retfunc 表下标（找不到返回 -1）
int  zh_tbl_retfunc_index(const char* name);

#ifdef __cplusplus
}
#endif
