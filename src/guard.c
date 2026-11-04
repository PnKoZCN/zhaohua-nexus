// ===========================================================================
// 回调崩溃隔离
//
// 问题：用户回调里访问野指针 → 整个游戏进程 SIGSEGV 退出。
// Mixin 里抛异常会被 catch，只打印堆栈；我们这边一个 bug 就带崩游戏。
//
// 方案：回调执行期间临时接管 SIGSEGV/SIGBUS，出错时 siglongjmp 回来。
//
// 代价：setjmp 约 20-50 ns。所以做成「可选」：
//   ii_install_guarded() 才启用，普通 ii_install() 零开销。
// ===========================================================================
#include "zh/instr_hook.h"
#include <signal.h>
#include <setjmp.h>
#include <string.h>
#include <unistd.h>

#define GUARD_MAX_DEPTH 4

// 每线程一个保护栈（回调可能嵌套）
//
// ★ 不能直接用 __thread：musl 会生成 R_AARCH64_TLSDESC 重定位，
//   Android bionic 不支持，整个 .so 会被拒绝加载（实测）。
//   改用 pthread_key，语义相同且无 TLSDESC。
#include <pthread.h>
#include <stdlib.h>

typedef struct {
    sigjmp_buf       jmp[GUARD_MAX_DEPTH];
    volatile int     depth;
    volatile int     active;
} zh_guard_tls;

static pthread_key_t  g_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void guard_tls_free(void* p) { free(p); }

static void guard_key_init(void) {
    pthread_key_create(&g_key, guard_tls_free);
}

static zh_guard_tls* guard_tls(void) {
    pthread_once(&g_once, guard_key_init);
    zh_guard_tls* t = (zh_guard_tls*)pthread_getspecific(g_key);
    if (!t) {
        t = (zh_guard_tls*)calloc(1, sizeof *t);
        if (t) pthread_setspecific(g_key, t);
    }
    return t;
}

static struct sigaction g_oldSegv;
static struct sigaction g_oldBus;
static volatile int  g_installed = 0;
static volatile long g_caught = 0;

static void guard_handler(int sig) {
    zh_guard_tls* t = guard_tls();
    if (t && t->active && t->depth > 0) {
        // 回调内部崩了 —— 跳回去，不终止进程
        __atomic_add_fetch(&g_caught, 1, __ATOMIC_RELAXED);
        t->active = 0;
        siglongjmp(t->jmp[t->depth - 1], 1);
    }
    // 不是回调里崩的 —— 交给原处理器（或默认行为）
    struct sigaction* old = (sig == SIGSEGV) ? &g_oldSegv : &g_oldBus;
    if (old->sa_handler == SIG_DFL || old->sa_handler == SIG_IGN) {
        signal(sig, SIG_DFL);
        raise(sig);
    } else if (old->sa_handler != NULL) {
        old->sa_handler(sig);
    } else if (old->sa_flags & SA_SIGINFO) {
        // SA_SIGINFO 的处理器无法从这里调用，退回默认
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static void install_once(void) {
    if (g_installed) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = guard_handler;
    sa.sa_flags   = SA_NODEFER;    // 允许在处理器里再次触发
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_oldSegv);
    sigaction(SIGBUS,  &sa, &g_oldBus);
    g_installed = 1;
}

// 在回调外面包一层。返回 1 = 回调正常返回，0 = 回调崩溃已隔离
int ii_guard_call(ii_callback cb, ii_ctx* c, void* user) {
    if (!cb) return 1;
    install_once();

    zh_guard_tls* t = guard_tls();
    if (!t) { cb(c, user); return 1; }        // 分配失败，退化成不保护

    if (t->depth >= GUARD_MAX_DEPTH) {
        // 嵌套太深（可能是回调自己递归），不过度保护
        cb(c, user);
        return 1;
    }

    int d = t->depth++;
    int ok = 1;
    if (sigsetjmp(t->jmp[d], 1) == 0) {
        t->active = 1;
        cb(c, user);
        t->active = 0;
    } else {
        ok = 0;                 // 从 guard_handler 跳回来
    }
    t->depth = d;
    return ok;
}

// 已隔离的崩溃次数
long ii_guard_caught(void) { return g_caught; }
