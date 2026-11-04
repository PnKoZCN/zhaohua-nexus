// t08c：用 guarded 模式的 hook，长时间运行，崩溃时能捕获
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd=-1;
static void P(const char* fmt,...){
    if(g_fd<0){ g_fd=open("/data/data/com.netease.x19/cache/zh_t08c.txt",
        O_WRONLY|O_CREAT|O_APPEND,0666); if(g_fd<0) return; }
    char b[320]; va_list a; va_start(a,fmt);
    int n=vsnprintf(b,sizeof b,fmt,a); va_end(a);
    if(n>0){ b[n++]='\n'; (void)!write(g_fd,b,n); }
}
static volatile unsigned long g_n=0;
static void cb(ii_ctx*c,void*u){(void)u;__atomic_add_fetch(&g_n,1,__ATOMIC_RELAXED);(void)c;}

__attribute__((constructor))
static void run(void){
    P("");
    P("════ t08c guarded 长跑 ════");
    if(zh_layer_init_default()!=ZH_OK && !zh_layer_ready()){P("FAIL init");return;}
    int r=zh_hook_guarded("LocalPlayer_normalTick", cb, NULL);
    P("  zh_hook_guarded rc=%d", r);
    for(int i=0;i<12;++i){
        sleep(2);
        P("  t=%ds n=%lu caught=%ld", (i+1)*2,
          __atomic_load_n(&g_n,__ATOMIC_RELAXED), zh_guard_caught());
    }
    P("  24 秒无崩溃 ✓");
    zh_unhook("LocalPlayer_normalTick");
    P("════ 结束 ════");
}
