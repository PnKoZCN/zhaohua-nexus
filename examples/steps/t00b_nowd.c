// t00b：初始化引擎但禁用 watchdog（setenv 后再 init）
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
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
    P("════ t00b 空载 + 无 watchdog ════");
    setenv("ZH_NO_WATCHDOG", "1", 1);
    P("  已设置 ZH_NO_WATCHDOG=1");
    int r = zh_layer_init_default();
    P("  init = %d  ready=%d", r, zh_layer_ready());
    for(int i=0;i<15;++i){
        sleep(2);
        P("  t=%ds 存活", (i+1)*2);
    }
    P("  30 秒无崩溃 ✓");
    P("════ 结束 ════");
}
