#ifdef ZH_PORTABLE
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// bionic 的 errno 展开成 `(*__errno())`。非 Android 环境没有这个符号，
// 这里用一个线程局部变量顶替 —— 只有引擎内部读写 errno，
// 不涉及与 libc 其他部分共享 errno 语义，够用。
static __thread int g_portable_errno;
int* __errno(void) { return &g_portable_errno; }

// bionic 的 atfork 注册；直接用 POSIX 的 pthread_atfork 实现。
int __register_atfork(void (*prepare)(void), void (*parent)(void),
                      void (*child)(void), void* dso_handle) {
    (void)dso_handle;
    return pthread_atfork(prepare, parent, child);
}

// Android 属性系统；非 Android 环境一律返回空串（引擎只用来读设备型号等）。
int __system_property_get(const char* name, char* value) {
    (void)name;
    if (value) value[0] = '\0';
    return 0;
}

#endif // ZH_PORTABLE
