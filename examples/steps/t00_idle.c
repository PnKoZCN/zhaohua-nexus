// t00：只初始化引擎，不装任何 hook，观察 30 秒
// 用来隔离"引擎自身的后台线程/池检测"是否是崩溃源
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd=-1;
static void P(const char* fmt,...){
    if(g_fd<0){ g_fd=open("/data/data/com.netease.x19/cache/zh_t00.txt",
        O_WRONLY|O_CREAT|O_APPEND,0666); if(g_fd<0) return; }
    char b[320]; va_list a; va_start(a,fmt);
    int n=vsnprintf(b,sizeof b,fmt,a); va_end(a);
    if(n>0){ b[n++]='\n'; (void)!write(g_fd,b,n); }
}
__attribute__((constructor))
static void run(void){
    P("");
    P("════ t00 空载（不装 hook）════");
    int r = zh_layer_init_default();
    P("  zh_layer_init_default = %d", r);
    P("  ready=%d base=0x%lx", zh_layer_ready(), zh_module_base());
    for(int i=0;i<15;++i){
        sleep(2);
        P("  t=%ds 存活", (i+1)*2);
    }
    P("  30 秒无崩溃 ✓");
    P("════ 结束 ════");
}
