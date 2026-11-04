// t13b：探针 —— 确认 Actor_getPos 是否被调用 + 常量插桩是否真改了指令
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd=-1;
static void P(const char* fmt,...){
    if(g_fd<0){ g_fd=open("/data/data/com.netease.x19/cache/zh_t13.txt",
        O_WRONLY|O_CREAT|O_APPEND,0666); if(g_fd<0) return; }
    char b[448]; va_list a; va_start(a,fmt);
    int n=vsnprintf(b,sizeof b,fmt,a); va_end(a);
    if(n>0){ b[n++]='\n'; (void)!write(g_fd,b,n); }
}
static volatile unsigned long g_entry=0, g_const=0;
static void cb_entry(ii_ctx*c, void*u){ (void)c;(void)u; __atomic_add_fetch(&g_entry,1,__ATOMIC_RELAXED); }
static void cb_const(ii_ctx*c, void*u){ (void)c;(void)u; __atomic_add_fetch(&g_const,1,__ATOMIC_RELAXED); }

__attribute__((constructor))
static void run(void){
    P("");
    P("════ t13b 探针 ════");
    if (zh_layer_init_default()!=ZH_OK && !zh_layer_ready()){ P("FAIL init"); return; }
    const char* fn="Actor_getPos";
    unsigned long a=zh_find(fn);
    P("  %s @ 0x%lx", fn, a);
    if(!a) return;

    // 1) 普通入口 hook，看这个函数到底跑不跑
    int r1=zh_hook(fn, cb_entry, NULL);
    P("  入口 hook rc=%d", r1);

    // 2) 常量插桩（+0x10, value=128）
    unsigned long ca=0;
    int ar=ii_const_addr(fn,128,0,&ca);
    P("  const128[0] addr=0x%lx (函数+0x%lx) rc=%d", ca, ca-a, ar);
    int r2=ii_install_at_const(fn,128,0,cb_const,NULL);
    P("  常量插桩 rc=%d", r2);

    P("  === 观察 3 秒 ===");
    for(int i=1;i<=3;++i){
        sleep(1);
        P("    t=%ds entry=%lu const=%lu", i,
          __atomic_load_n(&g_entry,__ATOMIC_RELAXED),
          __atomic_load_n(&g_const,__ATOMIC_RELAXED));
    }
    ii_uninstall(ca);
    zh_unhook(fn);
    P("════ t13b 结束 ════");
}
