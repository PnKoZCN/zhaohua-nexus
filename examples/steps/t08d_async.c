// t08d：正确的长跑测试写法 —— constructor 立即返回，工作放独立线程
//
// ★ 为什么要这样：
//   __attribute__((constructor)) 在 dlopen 时【同步执行】。
//   若在 constructor 里 sleep(30)，dlopen 会卡住调用它的线程；
//   如果那正好是游戏主循环/渲染线程，触摸事件 5 秒没人处理 → ANR → 闪退。
//   这是测试代码的设计错误，不是引擎缺陷。
//   正确做法：constructor 里只起线程，立刻返回。
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include "zh/zh_layer.h"

static int g_fd=-1;
static void P(const char* fmt,...){
    if(g_fd<0){ g_fd=open("/data/data/com.netease.x19/cache/zh_t08d.txt",
        O_WRONLY|O_CREAT|O_APPEND,0666); if(g_fd<0) return; }
    char b[320]; va_list a; va_start(a,fmt);
    int n=vsnprintf(b,sizeof b,fmt,a); va_end(a);
    if(n>0){ b[n++]='\n'; (void)!write(g_fd,b,n); }
}
static volatile unsigned long g_n=0;
static void cb(ii_ctx*c,void*u){(void)c;(void)u;__atomic_add_fetch(&g_n,1,__ATOMIC_RELAXED);}

static void* worker(void* arg) {
    (void)arg;
    P("");
    P("════ t08d 异步长跑 ════");
    P("  [worker] 线程已启动（constructor 早已返回）");
    if(zh_layer_init_default()!=ZH_OK && !zh_layer_ready()){P("FAIL init");return NULL;}
    int r=zh_hook("LocalPlayer_normalTick", cb, NULL);
    P("  zh_hook rc=%d 链=%d", r, ii_callback_count(zh_find("LocalPlayer_normalTick")));
    for(int i=0;i<15;++i){
        sleep(2);
        P("  t=%ds n=%lu", (i+1)*2, __atomic_load_n(&g_n,__ATOMIC_RELAXED));
    }
    P("  30 秒无崩溃 ✓");
    zh_unhook("LocalPlayer_normalTick");
    P("════ 结束 ════");
    return NULL;
}

__attribute__((constructor))
static void run(void){
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
    // ★ 立刻返回，不阻塞 dlopen 的调用线程
}
