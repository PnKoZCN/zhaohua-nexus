// ===========================================================================
// 非 Android 环境兼容层（ZH_PORTABLE=1 时启用）
//
// Android NDK 编译出的 .so 会引用几个 bionic 专有符号，普通 Linux
// （musl / glibc）没有它们，dlopen 会直接失败：
//
//   __errno                 bionic 里 errno 展开成 `(*__errno())`
//   __register_atfork       bionic 的 atfork 注册内部接口
//   __system_property_get   Android 属性系统
//
// 打开 ZH_PORTABLE 后，这里给出等价实现，让引擎可以在普通 Linux 上
// 加载 —— 用于在虚拟机 / CI 里做交叉验证，不必每次都上真机。
// ===========================================================================
#pragma once

#ifdef ZH_PORTABLE
#include <errno.h>
#include <stddef.h>
#include <string.h>

// bionic: #define errno (*__errno())
// 若编译时把 errno 展开成了 __errno()，这里补上实现。
int* __errno_impl(void);
#ifdef __cplusplus
extern "C" {
#endif
int* __errno(void);
int __register_atfork(void (*prepare)(void), void (*parent)(void),
                      void (*child)(void), void* dso_handle);
int __system_property_get(const char* name, char* value);
#ifdef __cplusplus
}
#endif

#endif // ZH_PORTABLE
