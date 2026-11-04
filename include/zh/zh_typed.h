// ===========================================================================
// 类型安全的 hook 宏（缺陷 #10 的解法）
//
// 问题：C 没有泛型，ii_callback(c, user) 里用户必须自己知道
//       「返回值在 x0 还是 q0」「第几个参数」「字段多大」。
//       写错了编译器不报错，运行期静默出错。
//
// 解法：用宏 + 类型化回调，把「读返回值/参数」这一步在编译期绑死。
//
//   // 声明一个 float 返回值的 hook
//   ZH_HOOK_RET_FLOAT("Actor_getHealth", on_health);
//   static void on_health(float hp, void* user) { ... }   ← 类型明确
//
// 覆盖不到的：字段偏移、cancel 语义。那些只能靠文档。
// ===========================================================================
#pragma once
#include "zh/instr_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- 返回值类型包装 ------------------------------------------------------
//
// 每个宏生成一个 _wrap 函数（把 ii_ctx 里的值按类型取出来），
// 用户只写类型明确的业务函数。

#define ZH_HOOK_RET(TYPE, CTYPE, GETEXPR, funcName, fn)                    \
    static void fn##_zhwrap(ii_ctx* c, void* u) {                           \
        CTYPE _v = (CTYPE)(GETEXPR);                                        \
        fn(_v, u);                                                          \
    }                                                                       \
    static int fn##_zhinstall(void) {                                       \
        return ii_install_at_return(funcName, -1, fn##_zhwrap, NULL);       \
    }

// 返回值是 int
#define ZH_HOOK_RET_INT(funcName, fn) \
    ZH_HOOK_RET(int, int64_t, ii_get_ret_int_h(c), funcName, fn)

// 返回值是 float（在 q0）
#define ZH_HOOK_RET_FLOAT(funcName, fn) \
    ZH_HOOK_RET(float, float, ii_get_ret_float_h(c), funcName, fn)

// 返回值是指针
#define ZH_HOOK_RET_PTR(TYPE, funcName, fn) \
    ZH_HOOK_RET(TYPE*, void*, ii_get_ret_ptr_h(c), funcName, fn)

// ---- 参数类型包装 --------------------------------------------------------
//
// 在函数入口 hook，把指定参数按类型取出来。

#define ZH_HOOK_ARG(TYPE, ARGIDX, funcName, fn)                            \
    static void fn##_zhwrap(ii_ctx* c, void* u) {                           \
        TYPE _a = (TYPE)(uintptr_t)c->x[ARGIDX];                            \
        fn(_a, u);                                                          \
    }                                                                       \
    static int fn##_zhinstall(void) {                                       \
        return ii_install(zh_find_h(funcName), fn##_zhwrap, NULL);          \
    }

// ---- 辅助：把引擎的取返回值函数暴露成可内联形式 --------------------------
//
// ii_get_ret_* 是 static inline，直接调用即可。
// 这里只是为了让宏里的表达式可读。

static inline int64_t ii_get_ret_int_h(ii_ctx* c)  { return (int64_t)c->x[0]; }

// ★ float 返回值在 q0 的低 32 位，必须「按位重解释」而不是数值转换。
//   写成 (float)c->q[0] 会把 __uint128_t 当整数转 float，得到完全错误的值
//   （实测 72.5 会变成 1116798976.0）。
static inline float ii_get_ret_float_h(ii_ctx* c) {
    union { __uint128_t q; float f; } u;
    u.q = 0;
    u.q = c->q[0];
    return u.f;                      // 取低 32 位
}
static inline double ii_get_ret_double_h(ii_ctx* c) {
    union { __uint128_t q; double d; } u;
    u.q = 0;
    u.q = c->q[0];
    return u.d;                      // 取低 64 位
}

static inline void* ii_get_ret_ptr_h(ii_ctx* c)  { return (void*)(uintptr_t)c->x[0]; }

// ---- 类型化设置返回值（编译期检查类型匹配）-------------------------------
//
// 之前：ii_set_ret_float(c, 100);   ← 100 是 int，隐式转 float，编译器可能不警告
// 现在：ZH_SET_RET_FLOAT(c, 100);   ← 同上但意图明确
//
#define ZH_SET_RET_INT(c, v)    ii_set_ret_int((c), (int64_t)(v))
#define ZH_SET_RET_FLOAT(c, v)  ii_set_ret_float((c), (float)(v))
#define ZH_SET_RET_PTR(c, v)    ii_set_ret_ptr((c), (void*)(v))

// ---- 字段访问的类型化宏 --------------------------------------------------
//
// 显式标注字段宽度，避免「用 4 读了一个 8 字节字段」。

#define ZH_FIELD_I8(c, arg, off)   ((int8_t)  ii_field_read((c),(arg),(off),1))
#define ZH_FIELD_I16(c, arg, off)  ((int16_t) ii_field_read((c),(arg),(off),2))
#define ZH_FIELD_I32(c, arg, off)  ((int32_t) ii_field_read((c),(arg),(off),4))
#define ZH_FIELD_I64(c, arg, off)  ((int64_t) ii_field_read((c),(arg),(off),8))
#define ZH_FIELD_PTR(c, arg, off)  ((void*)(uintptr_t)ii_field_read((c),(arg),(off),8))

#define ZH_FIELD_SET_I32(c, arg, off, v) ii_field_write((c),(arg),(off),4,(int64_t)(int32_t)(v))
#define ZH_FIELD_SET_I64(c, arg, off, v) ii_field_write((c),(arg),(off),8,(int64_t)(v))
#define ZH_FIELD_SET_PTR(c, arg, off, v) ii_field_write((c),(arg),(off),8,(int64_t)(uintptr_t)(v))

// 引擎里没有 zh_find_h，这里是占位（门面层有 zh_find）
// 用宏的项目通常同时 include zh_layer.h，直接用 zh_find
#ifndef ZH_NO_LAYER
#include "zh/zh_layer.h"
#define zh_find_h(n) zh_find(n)
#else
static inline unsigned long zh_find_h(const char* n) { (void)n; return 0; }
#endif

#ifdef __cplusplus
}
#endif
