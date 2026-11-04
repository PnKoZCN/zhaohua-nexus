#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include "zh/zh_layer.h"
static int fd=-1;
static void P(const char*f,...){ if(fd<0){fd=open("/data/data/com.netease.x19/cache/zh_rescue.txt",O_WRONLY|O_CREAT|O_TRUNC,0666);if(fd<0)return;}
  char b[256];va_list a;va_start(a,f);int n=vsnprintf(b,sizeof b,f,a);va_end(a);
  if(n>0){b[n++]='\n';(void)!write(fd,b,n);} }
__attribute__((constructor)) static void run(void){
    P("=== rescue ===");
    if (zh_layer_init_default()!=ZH_OK && !zh_layer_ready()){P("init fail");return;}
    unsigned long base=zh_module_base();
    const char* fn="Actor_getHealth";
    unsigned long a=zh_find(fn);
    P("entry=0x%lx (base+0x%lx)", a, a-base);
    int nret=ii_retpoint_count(fn);
    P("retpoints=%d", nret);
    int un=0;
    for(int i=0;i<nret;i++){
        unsigned long ra=0;
        if(ii_retpoint_addr(fn,i,&ra)!=0||!ra) continue;
        if(!ii_find(ra-4)) continue;
        unsigned int w=*(volatile unsigned int*)(ra-4);
        int rc=ii_uninstall(ra-4);
        P("  ret[%d] @0x%lx (0x%08x) -> rc=%d", i, ra-4, w, rc);
        if(rc==0) un++;
    }
    P("uninstalled=%d", un);
    P("=== rescue done ===");
}
