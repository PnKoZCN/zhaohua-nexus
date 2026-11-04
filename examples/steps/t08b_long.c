// t08b：t08 的长跑版 —— 3 个回调，跑 20 秒
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd=-1;
static void P(const char* fmt,...){
    if(g_fd<0){ g_fd=open("/data/data/com.netease.x19/cache/zh_t08b.txt",
        O_WRONLY|O_CREAT|O_APPEND,0666); if(g_fd<0) return; }
    char b[320]; va_list a; va_start(a,fmt);
    int n=vsnprintf(b,sizeof b,fmt,a); va_end(a);
    if(n>0){ b[n++]='\n'; (void)!write(g_fd,b,n); }
}
static volatile unsigned long hi=0,mid=0,lo=0;
static void cb_hi (ii_ctx*c,void*u){(void)c;(void)u;__atomic_add_fetch(&hi,1,__ATOMIC_RELAXED);}
static void cb_mid(ii_ctx*c,void*u){(void)c;(void)u;__atomic_add_fetch(&mid,1,__ATOMIC_RELAXED);}
static void cb_lo (ii_ctx*c,void*u){(void)c;(void)u;__atomic_add_fetch(&lo,1,__ATOMIC_RELAXED);}

__attribute__((constructor))
static void run(void){
    P("");
    P("════ t08b 长跑（3 回调 20 秒）════");
    if(zh_layer_init_default()!=ZH_OK && !zh_layer_ready()){P("FAIL init");return;}
    const char* fn="LocalPlayer_normalTick";
    unsigned long e=zh_find(fn);
    if(!e){P("FAIL 无函数");return;}

    int r1=zh_hook(fn, cb_mid, NULL);
    int r2=ii_add_callback(e, cb_hi, NULL, 300);
    int r3=ii_add_callback(e, cb_lo, NULL, 100);
    P("  install %d/%d/%d  链=%d", r1,r2,r3,ii_callback_count(e));

    for(int i=0;i<10;++i){
        sleep(2);
        P("  t=%ds hi=%lu mid=%lu lo=%lu 链=%d", (i+1)*2,
          __atomic_load_n(&hi,__ATOMIC_RELAXED),
          __atomic_load_n(&mid,__ATOMIC_RELAXED),
          __atomic_load_n(&lo,__ATOMIC_RELAXED),
          ii_callback_count(e));
    }
    P("  20 秒无崩溃 ✓");
    zh_unhook(fn);
    P("════ 结束 ════");
}
