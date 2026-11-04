// t11x：把 t11 拆成可停留的阶段，用宏只跑一种
// 编译时 -DPHASE=1|2|3
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

#ifndef PHASE
#define PHASE 1
#endif

static int g_fd=-1;
static void P(const char* fmt,...){
    if(g_fd<0){ g_fd=open("/data/data/com.netease.x19/cache/zh_t11x.txt",
        O_WRONLY|O_CREAT|O_APPEND,0666); if(g_fd<0) return; }
    char b[384]; va_list a; va_start(a,fmt);
    int n=vsnprintf(b,sizeof b,fmt,a); va_end(a);
    if(n>0){ b[n++]='\n'; (void)!write(g_fd,b,n); }
}
static volatile unsigned long g_a=0,g_b=0,g_g=0;
static void cbA(ii_ctx*c,void*u){(void)c;(void)u;__atomic_add_fetch(&g_a,1,__ATOMIC_RELAXED);}
static void cbB(ii_ctx*c,void*u){(void)c;(void)u;__atomic_add_fetch(&g_b,1,__ATOMIC_RELAXED);}
static void cbG(ii_ctx*c,void*u){(void)c;(void)u;__atomic_add_fetch(&g_g,1,__ATOMIC_RELAXED);}

__attribute__((constructor))
static void run(void){
    P("");
    P("════ t11x PHASE=%d ════", PHASE);
    if(zh_layer_init_default()!=ZH_OK && !zh_layer_ready()){P("FAIL init");return;}
    unsigned long entry=zh_find("LocalPlayer_normalTick");
    P("  entry=0x%lx", entry);
    if(!entry) return;

    ii_engine* A=ii_engine_create();
    ii_engine* B=ii_engine_create();
    P("  A=%p B=%p",(void*)A,(void*)B);
    if(!A||!B) return;

    int r1=ii_engine_install(A,entry,cbA,NULL);
    int r2=ii_engine_install(B,entry,cbB,NULL);
    int r3=zh_hook("LocalPlayer_normalTick",cbG,NULL);
    P("  install A=%d B=%d G=%d  链=%d", r1,r2,r3,ii_callback_count(entry));

#if PHASE >= 2
    P("  [PHASE%d] destroy(A)", PHASE);
    ii_engine_destroy(A);
    P("  [PHASE%d] destroy(A) 完成", PHASE);
#endif
#if PHASE >= 3
    P("  [PHASE%d] destroy(B)", PHASE);
    ii_engine_destroy(B);
    P("  [PHASE%d] destroy(B) 完成", PHASE);
#endif

    // 停留 40 秒，每 2 秒打一次
    for(int i=0;i<20;++i){
        sleep(2);
        P("  t=%ds A=%lu B=%lu G=%lu 链=%d", (i+1)*2,
          __atomic_load_n(&g_a,__ATOMIC_RELAXED),
          __atomic_load_n(&g_b,__ATOMIC_RELAXED),
          __atomic_load_n(&g_g,__ATOMIC_RELAXED),
          ii_callback_count(entry));
    }
    P("  [PHASE%d] 40 秒无崩溃 ✓", PHASE);
    zh_unhook("LocalPlayer_normalTick");
    P("════ PHASE%d 结束 ════", PHASE);
}
