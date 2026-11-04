// t11y：最小化 —— 只装 1 个回调，看是否崩溃
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd=-1;
static void P(const char* fmt,...){
    if(g_fd<0){ g_fd=open("/data/data/com.netease.x19/cache/zh_t11y.txt",
        O_WRONLY|O_CREAT|O_APPEND,0666); if(g_fd<0) return; }
    char b[384]; va_list a; va_start(a,fmt);
    int n=vsnprintf(b,sizeof b,fmt,a); va_end(a);
    if(n>0){ b[n++]='\n'; (void)!write(g_fd,b,n); }
}
static volatile unsigned long g_a=0;
static void cbA(ii_ctx*c,void*u){(void)c;(void)u;__atomic_add_fetch(&g_a,1,__ATOMIC_RELAXED);}

__attribute__((constructor))
static void run(void){
    P("");
    P("════ t11y 最小：1 个回调 ════");
    if(zh_layer_init_default()!=ZH_OK && !zh_layer_ready()){P("FAIL init");return;}
    unsigned long entry=zh_find("LocalPlayer_normalTick");
    P("  entry=0x%lx", entry);
    if(!entry) return;

    // 方式 1：直接用 zh_hook（t08 用过的方式）
    int r = zh_hook("LocalPlayer_normalTick", cbA, NULL);
    P("  zh_hook rc=%d 链=%d", r, ii_callback_count(entry));

    for(int i=0;i<15;++i){
        sleep(2);
        P("  t=%ds hits=%lu 链=%d", (i+1)*2,
          __atomic_load_n(&g_a,__ATOMIC_RELAXED), ii_callback_count(entry));
    }
    P("  30 秒无崩溃 ✓");
    zh_unhook("LocalPlayer_normalTick");
    P("════ t11y 结束 ════");
}
