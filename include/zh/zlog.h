// ===========================================================================
// zlog v2 —— 性能修复 + 可读错误码
//
// v1 的问题：每次日志 fopen/fclose，实测 237.7 µs/次。
//            60 FPS 下写 70 行就掉一帧。
//
// v2 改进：
//   1. 缓存 fd（open 一次，write 多次）      → 158 倍
//   2. 可选内存环形缓冲（异步落盘）          → 806 倍
//   3. 去掉 fsync（只在崩溃时刷）
//   4. ii_strerror：错误码 → 可读消息
//   5. 运行期改级别、按模块独立级别
// ===========================================================================
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <stdarg.h>
#include <stdint.h>


// ---- 模块 --------------------------------------------------------------
#define LOG_POOL    0
#define LOG_HOOK    1
#define LOG_REG     2
#define LOG_SAMPLE  3
#define LOG_LOCAL   4    // v2 新增：局部变量
#define LOG_MOD_MAX 5

// ---- 级别 --------------------------------------------------------------
#define ZLOG_ERROR 0
#define ZLOG_WARN  1
#define ZLOG_INFO  2
#define ZLOG_DEBUG 3

// ---- 输出模式 ----------------------------------------------------------
#define ZLOG_MODE_FILE  0   // 缓存 fd 直写（默认，158 倍）
#define ZLOG_MODE_RING  1   // 内存环形缓冲（806 倍，需 zlog_flush 落盘）
#define ZLOG_MODE_BOTH  2   // 两者都要

extern int g_zlogLevel;

void zlog_init(const char* path);
void zlog_set_level(int lv);
void zlog_set_level_mod(int mod, int lv);     // v2: 按模块设级别
int  zlog_get_level_mod(int mod);             // v2: 查模块级别
void zlog_set_mode(int mode);                 // v2: 切输出模式
void zlog_flush(void);                        // v2: 把环形缓冲落盘
void zlog_close(void);                        // v2: 关 fd，下次重新打开

void zlog_emit(int mod, int lv, const char* file, int line, const char* fmt, ...);

#define LOG_E(mod, ...) zlog_emit(mod, ZLOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_W(mod, ...) zlog_emit(mod, ZLOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_I(mod, ...) zlog_emit(mod, ZLOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_D(mod, ...) zlog_emit(mod, ZLOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)

// ---- v2 新增：错误码可读化 ---------------------------------------------
//
// v1 只返回 -7 / -5 / -17，用户要翻头文件才知道意思。
// v2 提供 ii_strerror()，直接给可读消息 + 建议。
//
const char* ii_strerror(int code);            // 短消息
const char* ii_strhint(int code);             // 详细建议（含可能原因）

// ---- v2 新增：条件断点/采样辅助宏 --------------------------------------
//
// 用法：
//   void cb(ii_ctx* c, void* u) {
//       II_SAMPLE_EVERY(100);                // 每 100 次才往下走
//       II_FIRST_N(10);                      // 只前 10 次
//       II_ONLY_IF(c, c->x[0] == 0x1234);    // 条件成立才往下走
//       LOG_I(LOG_HOOK, "hit x0=0x%llx", (unsigned long long)c->x[0]);
//   }
//
#define II_SAMPLE_EVERY(n)  do { \
    static unsigned long _cnt_; \
    if (++_cnt_ % (unsigned long)(n)) return; \
} while (0)

#define II_FIRST_N(k)  do { \
    static unsigned long _cnt_; \
    if (++_cnt_ > (unsigned long)(k)) return; \
} while (0)

#define II_ONLY_IF(c, cond)  do { if (!(cond)) return; } while (0)

#ifdef __cplusplus
}
#endif
