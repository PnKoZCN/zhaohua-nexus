// 数据表加载器测试
#include <stdio.h>
#include <string.h>
#include "zh/zh_db.h"

static int fails = 0;
#define CHECK(c,m) do { if(!(c)){printf("FAIL: %s\n",m);fails++;} else printf("  OK: %s\n",m);} while(0)

int main(int argc, char** argv) {
    printf("=== 数据表加载 ===\n");

    const char* path = (argc > 1) ? argv[1] : "zh_db.bin";
    zh_db db;
    int rc = zh_db_load(&db, path);
    printf("  加载 %s -> %d\n", path, rc);

    if (rc != ZH_DB_OK) {
        printf("  （跳过：需要先跑 tools/pack_db.py 生成数据文件）\n");
        return 0;   // 不算失败
    }

    char info[1024];
    zh_db_info(&db, info, sizeof info);
    printf("%s", info);

    CHECK(strlen(zh_db_game_version(&db)) > 0, "能读到游戏版本");

    // 各表
    const void* data; uint32_t cnt, esz;
    int n;

    n = zh_db_table(&db, ZH_TBL_RETTYPE, &data, &cnt, &esz);
    printf("  rettype: %d 条, entry=%u\n", n, esz);
    CHECK(n > 0 && esz == 16, "rettype 表可读");

    n = zh_db_table(&db, ZH_TBL_BLCALL, &data, &cnt, &esz);
    printf("  blcall: %d 条, entry=%u\n", n, esz);
    CHECK(n > 0, "blcall 表可读");

    n = zh_db_table(&db, ZH_TBL_POOLHINT, &data, &cnt, &esz);
    printf("  poolhint: %d 条, entry=%u\n", n, esz);
    CHECK(n > 0, "poolhint 表可读");

    // 不存在的表（funcinfo 没打包）
    n = zh_db_table(&db, ZH_TBL_FUNCINFO, &data, &cnt, &esz);
    CHECK(n == 0, "未打包的表返回 0");

    // 错误路径
    zh_db bad;
    CHECK(zh_db_load(&bad, "/nonexistent") == ZH_DB_E_OPEN, "不存在的文件");
    // 非 ZDB 文件：用临时文件，避免依赖 Android 上的固定路径
    // （之前硬编码 /sdcard/Download/logbench2，主机上不存在 → 误报失败）
    {
        const char* tp = "/tmp/zh_not_zdb.bin";
        FILE* tf = fopen(tp, "wb");
        int ok = 0;
        if (tf) {
            // ★ 必须 >= 文件头长度，否则 zh_db_load 会先判 ZH_DB_E_CORRUPT，
            //   根本走不到 magic 校验（这里要测的是 magic 不匹配）。
            unsigned char junk[128];
            memset(junk, 0x5A, sizeof junk);
            memcpy(junk, "NOTZ", 4);          // 故意不是 "ZDB1"
            fwrite(junk, 1, sizeof junk, tf);
            fclose(tf);
            ok = (zh_db_load(&bad, tp) == ZH_DB_E_MAGIC);
            remove(tp);
        }
        CHECK(ok, "非 ZDB 文件");
    }

    zh_db_unload(&db);
    CHECK(1, "卸载不崩");

    printf(fails ? "test_db: %d 项失败\n" : "test_db: 全部通过\n", fails);
    return fails ? 1 : 0;
}
