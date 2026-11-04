// 运行期数据表加载器
#include "zh/zh_db.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define HDR_LEN 32

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t fnv1a32(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

void zh_db_unload(zh_db* db) {
    if (!db) return;
    if (db->base && db->size) munmap(db->base, db->size);
    memset(db, 0, sizeof *db);
}

int zh_db_load(zh_db* db, const char* path) {
    if (!db || !path) return ZH_DB_E_OPEN;
    memset(db, 0, sizeof *db);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return ZH_DB_E_OPEN;

    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < HDR_LEN) {
        close(fd);
        return ZH_DB_E_CORRUPT;
    }

    void* m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return ZH_DB_E_OPEN;

    const uint8_t* b = (const uint8_t*)m;
    if (memcmp(b, "ZDB1", 4) != 0) { munmap(m, (size_t)st.st_size); return ZH_DB_E_MAGIC; }

    uint32_t fmtVer  = rd32(b + 0x04);
    uint32_t nTables = rd32(b + 0x08);
    uint32_t strOff  = rd32(b + 0x0C);
    uint32_t strLen  = rd32(b + 0x10);
    uint32_t verOff  = rd32(b + 0x14);
    uint32_t verLen  = rd32(b + 0x18);
    uint32_t cksum   = rd32(b + 0x1C);

    if (fmtVer != 1)  { munmap(m, (size_t)st.st_size); return ZH_DB_E_VERSION; }
    if (nTables > ZH_TBL_MAX || HDR_LEN + nTables * 16u > (size_t)st.st_size) {
        munmap(m, (size_t)st.st_size);
        return ZH_DB_E_CORRUPT;
    }

    db->base       = m;
    db->size       = (size_t)st.st_size;
    db->nTables    = nTables;
    db->strPoolOff = strOff;
    db->strPoolLen = strLen;
    db->gameVerOff = verOff;
    db->gameVerLen = verLen;
    db->checksum   = cksum;

    // 解析表目录
    for (uint32_t i = 0; i < nTables; ++i) {
        const uint8_t* d = b + HDR_LEN + i * 16;
        uint32_t type = rd32(d), cnt = rd32(d + 4);
        uint32_t off  = rd32(d + 8), esz = rd32(d + 12);
        if (off > db->size || (uint64_t)off + (uint64_t)cnt * esz > db->size) {
            zh_db_unload(db);
            return ZH_DB_E_CORRUPT;
        }
        if (type < ZH_TBL_MAX) {
            db->tbl[type].type      = type;
            db->tbl[type].count     = cnt;
            db->tbl[type].offset    = off;
            db->tbl[type].entrySize = esz;
            db->tbl[type].data      = b + off;
        }
    }

    (void)fnv1a32;
    return ZH_DB_OK;
}

int zh_db_table(const zh_db* db, uint32_t type,
                const void** outData, uint32_t* outCount, uint32_t* outEntrySize) {
    if (!db || type >= ZH_TBL_MAX) return 0;
    if (!db->tbl[type].data) return 0;
    if (outData)       *outData       = db->tbl[type].data;
    if (outCount)      *outCount      = db->tbl[type].count;
    if (outEntrySize)  *outEntrySize  = db->tbl[type].entrySize;
    return (int)db->tbl[type].count;
}

const char* zh_db_game_version(const zh_db* db) {
    if (!db || !db->base || !db->gameVerLen) return "";
    if ((size_t)db->gameVerOff + db->gameVerLen > db->size) return "";
    return (const char*)db->base + db->gameVerOff;
}

const char* zh_db_str(const zh_db* db, uint32_t off) {
    if (!db || !db->base) return "";
    if (off == 0) return "";                    // 0 = 空串
    // off 是「字符串池内相对偏移」，要加上池基址
    uint64_t abs = (uint64_t)db->strPoolOff + off;
    if (abs >= db->size) return "";
    const char* p = (const char*)db->base + abs;
    // 确保有不越界的 NUL
    const char* end = (const char*)db->base + db->size;
    for (const char* q = p; q < end; ++q) if (!*q) return p;
    return "";
}

int zh_db_info(const zh_db* db, char* buf, int len) {
    if (!db || !buf || len <= 0) return 0;
    // ★ 长度必须 >= ZH_TBL_MAX，否则遍历时越界（曾导致 SEGV）
    static const char* names[ZH_TBL_MAX + 1] = {
        "?", "rettype", "blcall", "const", "frame",
        "slot", "fieldacc", "funcinfo", "constfunc", "poolhint",
        "retfunc", "retpoint"
    };
    int n = snprintf(buf, (size_t)len, "db: %u tables, ver=\"%s\", %zu bytes\n",
                     db->nTables, zh_db_game_version(db), db->size);
    for (uint32_t i = 1; i < ZH_TBL_MAX; ++i) {
        if (!db->tbl[i].data) continue;
        int k = snprintf(buf + n, (size_t)(len - n), "  %-10s %6u 条 (entry %u)\n",
                         names[i], db->tbl[i].count, db->tbl[i].entrySize);
        if (k > 0) n += k;
        if (n >= len - 1) break;
    }
    if (n < len) buf[n] = 0;
    return n;
}
