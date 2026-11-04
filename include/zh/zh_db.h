// ===========================================================================
// 运行期数据表加载（缺陷 #5 的解法）
//
// 之前：数据表是 static const 数组，编译进 .so。
//   · const_db.h 就占 785 KB
//   · 游戏更新必须重新编译
//   · 无法同时支持多个版本
//
// 现在：数据表打包成二进制文件，运行期 mmap 加载。
//   · .so 体积大幅减小
//   · 换数据文件不用重编译
//   · 可按版本选文件
// ===========================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 表类型（与 pack_db.py 的常量一致）
#define ZH_TBL_RETTYPE    1
#define ZH_TBL_BLCALL     2
#define ZH_TBL_CONST      3
#define ZH_TBL_FRAME      4
#define ZH_TBL_SLOT       5
#define ZH_TBL_FIELDACC   6
#define ZH_TBL_FUNCINFO   7
#define ZH_TBL_CONSTFUNC  8
#define ZH_TBL_POOLHINT   9
#define ZH_TBL_RETFUNC   10
#define ZH_TBL_RETPOINT  11
#define ZH_TBL_MAX        12

// 加载结果
#define ZH_DB_OK           0
#define ZH_DB_E_OPEN      -1
#define ZH_DB_E_MAGIC     -2
#define ZH_DB_E_VERSION   -3
#define ZH_DB_E_CORRUPT   -4
#define ZH_DB_E_CHECKSUM  -5

typedef struct {
    void*     base;         // mmap 起始
    size_t    size;         // 文件大小
    uint32_t  nTables;
    uint32_t  strPoolOff;   // 字符串池起始（文件内绝对偏移）
    uint32_t  strPoolLen;
    uint32_t  gameVerOff;
    uint32_t  gameVerLen;
    uint32_t  checksum;
    struct {
        uint32_t type, count, offset, entrySize;
        const void* data;   // 已解析的指针（base + offset）
    } tbl[ZH_TBL_MAX];
} zh_db;

// 加载。成功返回 ZH_DB_OK，失败返回负错误码
int  zh_db_load(zh_db* db, const char* path);
void zh_db_unload(zh_db* db);

// 查某张表（返回条目数与数据指针）；没这张表返回 0
int  zh_db_table(const zh_db* db, uint32_t type,
                 const void** outData, uint32_t* outCount, uint32_t* outEntrySize);

// 游戏版本字符串（未设置返回 ""）
const char* zh_db_game_version(const zh_db* db);

// 取字符串池里的 C 字符串（off 是池内偏移）
const char* zh_db_str(const zh_db* db, uint32_t off);

// 诊断
int zh_db_info(const zh_db* db, char* buf, int len);

#ifdef __cplusplus
}
#endif
