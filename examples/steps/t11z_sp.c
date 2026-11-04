// t11z：检查栈是否随调用次数漂移
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd=-1;
static void P(const char* fmt,...){
    if(g_fd<0){ g_fd=open("/data/data/com.netease.x19/cache/zh_t11z.txt",
        O_WRONLY|O_CREAT|O_APPEND,0666); if(g_fd<0) return; }
    char b[320]; va_list a; va_start(a,fmt);
    int n=vsnprintf(b,sizeof b,fmt,a); va_end(a);
    if(n>0){ b[n++]='\n'; (void)!write(g_fd,b,n); }
}
static volatile unsigned long g_n=0;
static void cb(ii_ctx* c, void* u){
    (void)u;
    unsigned long n = __atomic_add_fetch(&g_n,1,__ATOMIC_RELAXED);
    // 每 50 次记一次 ctx->sp，看是否单调漂移
    if (n % 50 == 1) P("  n=%lu ctx.sp=0x%llx ctx.x30=0x%llx",
                       n, (unsigned long long)c->sp, (unsigned long long)c->x[30]);
}

__attribute__((constructor))
static void run(void){
    P("");
    P("════ t11z 栈漂移检查 ════");
    if(zh_layer_init_default()!=ZH_OK && !zh_layer_ready()){P("FAIL init");return;}
    int r = zh_hook("LocalPlayer_normalTick", cb, NULL);
    P("  zh_hook rc=%d", r);
    sleep(20);
    P("  20 秒无崩溃 n=%lu", __atomic_load_n(&g_n,__ATOMIC_RELAXED));
    zh_unhook("LocalPlayer_normalTick");
    P("════ 结束 ════");
}
