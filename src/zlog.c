// zlog v2 实现
#include "zh/zlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/uio.h>

#define RING_SIZE (1024*1024)

int  g_zlogLevel = ZLOG_INFO;
static int  g_modLevel[LOG_MOD_MAX] = {2,2,2,2,2};
static int  g_mode = ZLOG_MODE_FILE;

static char g_zlogPath[256] = "/data/data/com.netease.x19/files/zhprobe/zh.log";
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

static int   g_fd = -1;
static int   g_fdTried = 0;

// 环形缓冲
static char   g_ring[RING_SIZE];
static size_t g_ringPos = 0;
static size_t g_ringLost = 0;

static const char* modName(int m) {
    static const char* n[] = {"POOL","HOOK","REG","SAMPLE","LOCAL"};
    return (m >= 0 && m < LOG_MOD_MAX) ? n[m] : "?";
}
static const char* lvName(int l) {
    static const char* n[] = {"E","W","I","D"};
    return (l >= 0 && l <= 3) ? n[l] : "?";
}

static int ensure_fd(void) {
    if (g_fd >= 0) return g_fd;
    if (g_fdTried && g_fd < 0) return -1;
    g_fdTried = 1;
    g_fd = open(g_zlogPath, O_WRONLY|O_CREAT|O_APPEND, 0600);
    return g_fd;
}

void zlog_init(const char* path) {
    pthread_mutex_lock(&g_mx);
    if (path) {
        strncpy(g_zlogPath, path, sizeof(g_zlogPath)-1);
        g_zlogPath[sizeof(g_zlogPath)-1] = 0;
    }
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    g_fdTried = 0;
    pthread_mutex_unlock(&g_mx);
}
void zlog_set_level(int lv) { g_zlogLevel = lv; }
void zlog_set_level_mod(int mod, int lv) {
    if (mod >= 0 && mod < LOG_MOD_MAX) g_modLevel[mod] = lv;
}
int zlog_get_level_mod(int mod) {
    return (mod >= 0 && mod < LOG_MOD_MAX) ? g_modLevel[mod] : ZLOG_INFO;
}
void zlog_set_mode(int mode) { g_mode = mode; }

void zlog_close(void) {
    pthread_mutex_lock(&g_mx);
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    g_fdTried = 0;
    pthread_mutex_unlock(&g_mx);
}

void zlog_flush(void) {
    pthread_mutex_lock(&g_mx);
    int fd = ensure_fd();
    if (fd >= 0 && g_ringPos > 0) {
        size_t off = 0;
        while (off < g_ringPos) {
            ssize_t w = write(fd, g_ring + off, g_ringPos - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
        g_ringPos = 0;
    }
    pthread_mutex_unlock(&g_mx);
}

void zlog_emit(int mod, int lv, const char* file, int line, const char* fmt, ...) {
    // 级别检查（先做，避免加锁）
    int lim = g_zlogLevel;
    if (mod >= 0 && mod < LOG_MOD_MAX && g_modLevel[mod] < lim) lim = g_modLevel[mod];
    if (lv > lim) return;

    char buf[512];
    int n = 0;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long ms = (unsigned long)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);

    n += snprintf(buf, sizeof(buf), "%lu [%s][%s] ",
                  ms, modName(mod), lvName(lv));

    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + n, sizeof(buf) - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (k > 0) n += k;
    if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;

    if (lv == ZLOG_DEBUG && file) {
        const char* b = strrchr(file, '/');
        n += snprintf(buf + n, sizeof(buf) - (size_t)n - 2, "  (%s:%d)",
                      b ? b + 1 : file, line);
        if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
    }
    buf[n++] = '\n';

    pthread_mutex_lock(&g_mx);

    if (g_mode == ZLOG_MODE_RING || g_mode == ZLOG_MODE_BOTH) {
        if (g_ringPos + (size_t)n <= sizeof(g_ring)) {
            memcpy(g_ring + g_ringPos, buf, (size_t)n);
            g_ringPos += (size_t)n;
        } else {
            g_ringLost += (size_t)n;
        }
    }

    if (g_mode == ZLOG_MODE_FILE || g_mode == ZLOG_MODE_BOTH) {
        int fd = ensure_fd();
        if (fd >= 0) {
            size_t off = 0;
            while (off < (size_t)n) {
                ssize_t w = write(fd, buf + off, (size_t)n - off);
                if (w <= 0) break;
                off += (size_t)w;
            }
        }
    }

    pthread_mutex_unlock(&g_mx);
}

// ---- 错误码可读化 ------------------------------------------------------

const char* ii_strerror(int code) {
    switch (code) {
        case   0: return "成功";
        case  -1: return "参数无效";
        case  -2: return "该地址已安装过";
        case  -3: return "站点表已满";
        case  -5: return "该地址不可插桩";
        case  -7: return "16 字节覆盖不安全";
        case -10: return "改写入口失败";
        case -11: return "跳转编码失败（超出 ±128MB）";
        case -13: return "后续指令被污染";
        case -15: return "无可用近处池（LIGHT/重定向）";
        case -16: return "站点类型冲突（FULL/LIGHT 不能混挂）";
        case -17: return "暂停线程超时";
        case -18: return "无可用近处池（FULL/RAW）";
        default:  return code > 0 ? "成功（正数）" : "未知错误";
    }
}

const char* ii_strhint(int code) {
    switch (code) {
        case  -2: return "先用 ii_uninstall 卸载，或换一个插桩点";
        case  -3: return "调用 ii_pool_stats() 看池占用；考虑增大池或卸载不用的站点";
        case  -5: return "原因通常是：地址不在 .text / 指令是 ldr-literal / "
                        "指令使用了 x16 或 x17。用 ii_can_instrument 预检，"
                        "换一条相邻指令（前后 4 字节）";
        case  -7: return "16 字节覆盖范围内有分支/PC 相对指令，或碰了 x16/x17。"
                        "改用 4 字节模式（ii_install）通常能过";
        case -11: return "目标距离超过 ±128MB。检查 base 是否算错，"
                        "或该 .so 映射位置异常";
        case -13: return "桩点后 N 条指令使用了 x16/x17，跳回会破坏它们。"
                        "换一个插桩点";
        case -15: return "附近没有 4KB 对齐的空闲页。可尝试 ii_register_hint_pools "
                        "预先注册已知池位置";
        case -16: return "同一地址先卸掉现有 handler 再装新的";
        case -17: return "线程数过多导致 300ms 内停不完。减少后台线程，"
                        "或换 4 字节模式（不需要暂停）";
        case -18: return "同 -15";
        default:  return "";
    }
}

