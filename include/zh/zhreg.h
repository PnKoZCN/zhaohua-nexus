// ===========================================================================
// zhreg —— 跨探针共享的插桩注册表
//
// 问题：多个探针 .so 同时加载到同一进程时，各有独立的静态变量：
//   g_pools / g_nPool / g_sites / watchdog 线程
// 结果是 A 注册的池 B 不知道，池死了没人管，两个探针可能写到同一个槽位。
//
// 方案：用 mmap 一个固定文件做共享区。同一进程内所有探针映射同一块内存，
//       池分配、站点登记、watchdog 归属全部走这块共享区。
//
// 进程重启检测：pid + /proc/self/stat 的 starttime
//   不匹配 → 整块重置（上次进程的地址全部失效）
// ===========================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZHREG_MAGIC        0x5A4852454702ULL   // "ZHREG"+v2：站点结构加了回调链，旧布局不兼容
// 共享区布局版本：本文件的结构体/上限一变就必须 +1。
// 老探针看到版本不符会整块重置（进程内共用一份布局，混跑必然读错偏移）。
#define ZHREG_LAYOUT       2
#define ZHREG_PATH         "/data/data/com.netease.x19/files/zhprobe/poolreg.bin"
// 共享区大小必须 >= sizeof(zh_region)，否则 mmap 后写越界（会踩到相邻映射）。
// 当前布局：pool[64] 约 380 KB + site[8192] 约 8.5 MB，取 12 MB 留余量。
// 改动任何上限后都要重新核对下面这条静态断言。
#define ZHREG_FILE_SIZE    (12*1024*1024)
#define ZHREG_MAX_POOL     64
#define ZHREG_MAX_SITE     8192   // 跨探针共享：多个库都在装站点时是共用的
#define ZHREG_SITES_PER_POOL 256   // 每个池记多少站点地址（池失效迁移用）
// 单个站点同时挂几个探针的回调（跨探针上限）。
// 第 n 个库在同一个函数上再挂一个回调就 +1。16 个库封顶。
#define ZHREG_MAX_CB       32

#define ZHREG_MAX_FREE 16      // 每个池记住多少个已释放槽位可复用（必须在 typedef 之前）
#define ZHREG_QUARANTINE_MS 3000   // 归还的 shell 槽位隔离多久才允许复用
#define ZHREG_MAX_PENDING   32     // 每个池最多记多少个"隔离中"的槽位
#define ZHREG_WD_TIMEOUT_MS 10000

// ---------------------------------------------------------------------------
// 跨探针上限说明（数值在文件开头统一给出）
// ---------------------------------------------------------------------------
//
// 这些上限决定「同时注入几个库」时的实际容量。改动它们会改变共享区
// 布局（zh_region 大小），因此 ZHREG_LAYOUT 必须同步 +1：老版本探针
// 看到布局版本不符会整块重置，而不是按错的偏移去读别人的数据。
//
//   ZHREG_MAX_CB         单个站点同时挂几个探针的回调（当前 32）
//   ZHREG_MAX_SITE       全进程跨探针的站点总数（当前 8192）
//   ZHREG_MAX_POOL       trampoline 池总数（当前 64）
//   ZHREG_SITES_PER_POOL 每个池记录多少站点地址（当前 256）
//
// 每个探针另有自己的本地上限（II_MAX_SITES / II_ENGINE_MAX_OWN），
// 是第二道闸：共享区够用不代表单库可以无限装。


typedef struct {
    uint64_t      base;
    uint64_t      size;
    uint64_t      used;
    uint32_t      guard;
    int32_t       state;        // 0=free 1=active 2=dead
    int32_t       owner;        // 注册它的探针 ID
    int32_t       nSite;
    unsigned long siteAddr[ZHREG_SITES_PER_POOL];
    char          kind[16];
    // 已释放的槽位，分配时优先复用。
    // used 只增不减会让反复装/卸耗尽池（308KB/64 ≈ 4900 个），
    // 这里记最近释放的 16 个足够覆盖正常装卸场景。
    // 隔离区：刚归还的 shell 槽位不能立刻复用。
    // 卸载时我们用 4 字节原子写把入口恢复成原指令，但此刻可能还有线程
    // 已经跳进了 shell、还没执行完。若这个槽位马上被下一次 install 拿去
    // 写新代码，那些线程就会跑到别人的 shell 里 —— 参数错位、死循环、
    // 最终整进程闪退（t11 真机现象：destroy 后几秒卡死）。
    // 所以归还的槽位先记归还时刻，至少隔离 ZHREG_QUARANTINE_MS 才可复用。
    uint64_t      freeSlot[ZHREG_MAX_FREE];
    int32_t       nFreeSlot;
    int32_t       padFree;
    // 1 = 这个池不是我们 mmap 的（来自 poolhint 指向的游戏 .bss 空隙），
    //     因此绝不能往池首写 guard 标记，存活检测改用 mincore。
    int32_t       noGuard;
    int32_t       padNg;
    // 隔离中的槽位（已归还但还在冷却）
    uint64_t      pendSlot[ZHREG_MAX_PENDING];
    uint64_t      pendMs[ZHREG_MAX_PENDING];
    int32_t       nPend;
    int32_t       padPend;
} zh_pool_rec;

// 回调链槽位：同一站点可以被多个探针同时挂回调
// cb 是函数指针，只在同一进程内有效（跨 .so 但同进程，可以直接调）
typedef struct {
    uint64_t cb;        // 回调函数指针
    uint64_t user;      // 用户数据
    int32_t  owner;     // 注册它的探针实例 ID
    int32_t  alive;     // 0 = 槽位空（探针退出后可回收）
    int32_t  priority;  // 越大越先执行（对应 Mixin 的 priority）
    uint32_t pad;
} zh_cb_rec;            // 32 字节（布局未变：priority 占用了原来的 pad）

// ZHREG_MAX_CB 定义见文件末尾（跨探针容量段），此处不再重复定义。

// 站点记录
//
// kind 由「第一个安装者」决定：FULL(ii_ctx) 还是 LIGHT(ii_ctx_light)。
// 后加入的探针必须用同一种 handler 类型，否则 ctx 布局对不上，
// 回调会读到错位的寄存器。
typedef struct {
    unsigned long addr;
    uint32_t      orig;
    int32_t       owner;      // 建立站点的探针（它负责 enable/disable）
    int32_t       poolIdx;
    int32_t       alive;
    int32_t       kind;       // 0 = FULL(ii_ctx) 1 = LIGHT(ii_ctx_light)
    int32_t       nCb;        // 已用回调槽数
    int32_t       pad;
    zh_cb_rec     cb[ZHREG_MAX_CB];
} zh_site_rec;

typedef struct {
    volatile uint32_t lock;
    uint32_t          pad0;
    uint64_t          magic;
    int32_t           pid;
    uint32_t          pad1;
    uint64_t          procStartTime;
    uint32_t          wdOwner;          // 0 = 没人负责 watch
    uint32_t          pad2;
    volatile uint64_t wdHeartbeat;      // ms 时间戳
    int32_t           nOwner;
    int32_t           nPool;
    int32_t           nSite;
    int32_t           pad3;
    zh_pool_rec pool[ZHREG_MAX_POOL];
    zh_site_rec site[ZHREG_MAX_SITE];
} zh_region;

// 附加到共享区（首次调用 mmap；进程重启自动重置）。失败返回 NULL。
zh_region* zhreg_attach(void);

// 本探针实例 ID（1 起）。未 attach 返回 0。
int  zhreg_owner(void);

// 自旋锁（跨 so，同进程）
void zhreg_lock(zh_region* r);
void zhreg_unlock(zh_region* r);

// 池：注册 / 分配槽位 / 校验
int   zhreg_add_pool(zh_region* r, uint64_t base, uint64_t size, const char* kind, int owner);
void* zhreg_alloc_slot(zh_region* r, uint64_t target, uint64_t slotSize,
                       int* poolIdxOut, int owner);
int   zhreg_note_site(zh_region* r, int poolIdx, unsigned long addr);
// 归还一个槽位（site_release 时调用），供后续分配复用
int   zhreg_free_slot(zh_region* r, int poolIdx, uint64_t slotAddr);
int   zhreg_pool_alive(zh_region* r, int idx);

// watchdog 归属
int  zhreg_wd_claim(zh_region* r, int owner);
void zhreg_wd_beat(zh_region* r);
int  zhreg_wd_takeover(zh_region* r, int owner);
int  zhreg_wd_should_takeover(zh_region* r);
int  zhreg_wd_is_mine(zh_region* r, int owner);

// 站点登记
// kind: 0 = FULL, 1 = LIGHT
int  zhreg_add_site(zh_region* r, unsigned long addr, uint32_t orig, int poolIdx,
                    int owner, int kind);
// 只删「该地址 + 该 owner」的那条活记录。
// 不能只按 addr 匹配：同一地址可能先后被多个探针装过（历史记录 alive=0 还留在表里），
// 只按 addr 会命中那条死记录，真正活着的那条永远删不掉。
void zhreg_del_site(zh_region* r, unsigned long addr, int owner);

// ---- 多探针共享同一站点 ----
// 查某地址是否已有活站点，返回索引，无则 -1
int  zhreg_find_site(zh_region* r, unsigned long addr);
// 往站点追加一个回调（同地址第二次安装走这里，而不是拒绝）
// 返回槽位索引；-2 = 站点不存在/已死；-3 = 槽位满
// priority 越大越先执行；相同则按注册顺序。链按 priority 降序维护。
int  zhreg_add_cb(zh_region* r, int siteIdx, uint64_t cb, uint64_t user,
                  int32_t owner, int32_t priority);
// 撤销本探针在该站点上的回调
void zhreg_del_cb(zh_region* r, int siteIdx, int32_t owner);
// 站点上属于「别人」的回调数（供 handler 快速判断要不要进慢路径）
int  zhreg_foreign_cb(zh_region* r, int siteIdx, int32_t myId);

// 同一探针在某个站点里占用的回调槽数
int  zhreg_owner_cb_count(zh_region* r, int siteIdx, int32_t owner);

#ifdef __cplusplus
}
#endif
