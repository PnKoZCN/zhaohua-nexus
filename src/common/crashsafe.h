// async-signal-safe 的崩溃转储工具
//
// POSIX 保证在信号处理器里安全的只有：
//   write, _exit, sigaction, kill, read, open, close, lseek, ...
// 不安全的（原实现用到的）：
//   snprintf（可能 malloc / 加 stdio 锁）
//   signal（已被 sigaction 取代）
//   raise（走 pthread_kill，可能加锁）
//
// 用本文件替代，避免崩溃时死锁导致 tombstone 无法生成。
#pragma once
#include <stdint.h>
#include <unistd.h>

// 把 v 转成十六进制写入 out（不含 0x 前缀），返回写入字节数
static inline int zh_sh_hex(uint64_t v, char* out) {
    static const char H[] = "0123456789abcdef";
    char t[16];
    int i = 0, n = 0;
    if (!v) { out[0] = '0'; return 1; }
    while (v && i < 16) { t[i++] = H[v & 15]; v >>= 4; }
    while (i > 0) out[n++] = t[--i];
    return n;
}

// 十进制（仅 32 位无符号）
static inline int zh_sh_dec(uint32_t v, char* out) {
    char t[10];
    int i = 0, n = 0;
    if (!v) { out[0] = '0'; return 1; }
    while (v && i < 10) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) out[n++] = t[--i];
    return n;
}

// 追加字符串，返回新长度
static inline int zh_sh_str(const char* s, char* out, int n) {
    while (*s) out[n++] = *s++;
    return n;
}

// 直接写 fd 2，忽略错误
static inline void zh_sh_emit(const char* buf, int n) {
    int off = 0;
    while (off < n) {
        ssize_t w = write(2, buf + off, (size_t)(n - off));
        if (w <= 0) break;
        off += (int)w;
    }
}
