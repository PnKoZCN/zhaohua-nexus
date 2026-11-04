# 指南：常见陷阱

真机上踩过的坑，全部有据可查。

---

## 陷阱 1：在 `constructor` 里阻塞 ★★★

### 现象

**注入后 6 秒 → 闪退，无 tombstone，logcat 有 ANR。**

```
Input dispatching timed out (Waited 5000ms for MotionEvent ...)
```

### 原因

**`__attribute__((constructor))` 在 `dlopen` 内部同步执行。**

```c
__attribute__((constructor))
static void init(void) {
    zh_layer_init_default();
    zh_hook(...);
    while (1) sleep(5);      // ★ 这里阻塞了 dlopen 的调用者
}
```

**如果注入线程是游戏主线程/渲染线程 → 整个游戏卡住 → ANR → 被杀。**

### 修复

```c
__attribute__((constructor))
static void init(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) {
        pthread_detach(t);   // ★ 立刻返回
    }
}
```

**`constructor` 只做"启动线程"这一件事，其他全在 worker 里。**

### 代价

**这条坑让整个项目走了 20 多轮弯路。** 之前一直以为是引擎的 hook 有问题。

---

## 陷阱 2：在回调里做重活 ★★★

### 现象

**游戏卡顿，帧率跌到个位数，或者直接 ANR。**

### 原因

**回调运行在调用者的线程上（通常是游戏主线程）。**

```c
static void on_tick(ii_ctx* c, void* user) {
    (void)c; (void)user;

    // 这些都会冻结游戏
    FILE* f = fopen("/sdcard/log.txt", "a");
    fprintf(f, "tick\n");
    fclose(f);

    usleep(10000);       // ★ 睡 10 ms × 20 TPS = 20% CPU
    do_heavy_math();
}
```

### 修复

**回调里只做 O(1) 的事，重活丢给工作线程：**

```c
static volatile int g_pending = 0;

static void on_tick(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_store_n(&g_pending, 1, __ATOMIC_RELAXED);   // 只设标志
}

static void* worker(void* arg) {
    (void)arg;
    while (1) {
        usleep(50000);
        if (__atomic_exchange_n(&g_pending, 0, __ATOMIC_RELAXED)) {
            // 在这里做重活
        }
    }
    return NULL;
}
```

### 引擎的告警

**回调超过 50 ms 会打日志：**

```
[SLOW] 回调 @0x7c45db46ac 耗时 82 ms（阈值 50）。回调运行在游戏线程上，
       重操作会冻结游戏，请改为异步。
```

**看到这个就去改。**

---

## 陷阱 3：库卸载后站点还在 ★★★

### 现象

**`dlclose` 后游戏下次调用该函数 → 闪退，无 tombstone。**

### 原因

**站点的 shell 在你的 `.so` 里（或由你的 `.so` 分配）。**

```
游戏函数 → b shell → shell 里是 ldr x16, #8
                     ↓
                   指向 ii_common（在你的 .so 里，已卸载 = 野内存）
```

### 修复

```c
__attribute__((destructor))
static void cleanup(void) {
    zh_unhook_all();       // ★ 必须
    zh_guard_off();
}
```

**或者按 owner 精确清理**：

```c
__attribute__((destructor))
static void cleanup(void) {
    zh_unhook_owner("my_mod");
}
```

---

## 陷阱 4：一个地址装两次 ★★

### 现象

`zh_hook` 返回 `-2`，你的第二个回调根本没生效。

### 原因

**一个地址只能有一个站点。**

```c
zh_hook("func", cb_a, NULL);   // 建站点
zh_hook("func", cb_b, NULL);   // 返回 -2，cb_b 没装
```

### 修复

```c
int rc = zh_hook(fn, cb_b, NULL);
if (rc == -2) {
    ii_add_callback(zh_find(fn), cb_b, NULL, 0);
}
```

---

## 陷阱 5：返回点读不到参数 ★★

### 现象

**返回点 hook 里 `c->x[0]` 是返回值，不是 `this`。**

### 原因

**`x0` 在函数返回时被返回值覆盖了。**

```c
static void on_ret(ii_ctx* c, void* user) {
    void* p_this = (void*)c->x[0];   // ★ 这是返回值，不是 this
}
```

### 修复

**要么在入口保存，要么用返回点只处理返回值：**

```c
static __thread void* t_saved_this = NULL;   // 线程局部

static void on_entry(ii_ctx* c, void* user) {
    (void)user;
    t_saved_this = (void*)c->x[0];
}

static void on_ret(ii_ctx* c, void* user) {
    (void)user;
    int32_t ret = zh_get_ret_i32(c);
    LOG_I(LOG_HOOK, "this=%p ret=%d", t_saved_this, ret);
}
```

**用 `__thread` 避免多线程串数据。**

---

## 陷阱 6：返回点 cancel 破坏栈帧 ★★★

### 现象

**闪退，tombstone 指向游戏代码，`sp` 是一个不合理的值。**

### 原因

**返回点处 `callee-saved` 寄存器已被函数体恢复，但 `sp` 还没调整到位。**

**在这个位置 cancel = 直接 `ret`，跳过了后续的栈平衡指令。**

### 修复

**返回点不要 cancel。**

```c
static void on_ret(ii_ctx* c, void* user) {
    // ii_cancel(c);   ← ★ 绝对不要
    zh_set_ret_i32(c, 0);   // 只改返回值
}
```

**要"跳过整个函数"，用入口 hook + cancel。**

---

## 陷阱 7：往游戏内存写 guard ★★★

### 现象

**hook 装上，几秒后卡死闪退，无 tombstone。**

### 原因

**`zhreg_add_pool` 早期版本对所有池都写 guard：**

```c
*(volatile uint32_t*)base = p->guard;   // ← 往游戏 .bss 写
```

**`hint` 池指向游戏自己的 `.bss` → 改了游戏数据 → 几秒后某个逻辑读到垃圾 → 崩在很远的地方。**

### 修复

```c
int ownMem = (kind && strcmp(kind, "own") == 0);
p->noGuard = ownMem ? 0 : 1;
if (ownMem) {
    *(volatile uint32_t*)base = p->guard;   // 只有自己的内存才写
}
```

**存活检测改用 `mincore()`：**

```c
if (p->noGuard) {
    unsigned char vec = 0;
    if (mincore((void*)p->base, 4096, &vec) != 0) return 0;
    return 1;
}
```

---

## 陷阱 8：热路径 UAF ★★★

### 现象

**卸载一个回调后，游戏几秒内闪退。**

### 原因

**`chain_remove` 直接 `free(n)`：**

```
线程 A: 遍历链，正在调 n->cb
线程 B: chain_remove → free(n)
线程 A: n = n->next   ← 读已释放内存
```

### 修复

**懒删除：**

```c
n->alive = 0;    // 只标记
```

**遍历时跳过：**

```c
for (ii_cb_node* n = s->cbs; n; n = n->next) {
    if (!n->alive || !n->cb) continue;
    n->cb(c, n->user);
}
```

**真正的 free 只在站点整体销毁时做。**

---

## 陷阱 9：大数组放栈上 ★★

### 现象

**游戏工作线程崩溃，栈指针跑到非法地址。**

### 原因

```c
void ii_uninstall_all(void) {
    unsigned long addrs[II_MAX_SITES];   // 262144 × 8 = 2 MB 栈！
    ...
}
```

**游戏工作线程栈只有 1~2 MB。**

### 修复

```c
unsigned long* addrs = malloc(total * sizeof(unsigned long));
if (!addrs) return;
...
free(addrs);
```

**规则**：**栈上不要放超过 64 KB 的数组。**

---

## 陷阱 10：暂停状态泄漏 ★★★

### 现象

**所有游戏线程永久卡死，无响应。**

### 原因

**`suspend_others()` 重复调用时 `g_pauseDepth` 没归零，`resume_others()` 的配对数不够：**

```c
// 错误
suspend_others();   // depth = 1
suspend_others();   // depth = 2
resume_others();    // depth = 1  ← 只恢复一次，还剩 1
// 线程还在暂停状态
```

### 修复

**严格配对 + 强制兜底：**

```c
void zh_stop_wake_all(void) {
    g_stopReq = 0;
    g_pauseDepth = 0;
    // 唤醒所有在 do_signal_stop 里等的线程
    for (each paused thread) {
        tgkill(pid, tid, SIGCONT);
    }
}
```

**`ii_uninstall_all` 里每次循环都调：**

```c
zh_stop_wake_all();
g_pauseDepth = 0;
g_stopReq = 0;
```

---

## 陷阱 11：0 长度文件 ★

### 现象

`zh_tbl_load` 返回错误，或读越界。

### 原因

```c
// 测试代码传了 12 字节的文件
// 但最小需要 HDR_LEN(32) + nTables*24 = 272 字节
```

### 修复

```c
if (fileLen < HDR_LEN) return ZH_E_ARG;
if (fileLen < hdr->hdrLen + hdr->nTables * 24) return ZH_E_ARG;
```

---

## 陷阱 12：结构体打包宽度不一致 ★★★

### 现象

**整张表数据错位，所有字段都是垃圾。**

### 原因

```python
# pack_db.py
(5, 'fieldacc', '<IHBBBB'),   # 10 字节
```

```c
// C 结构体
typedef struct {
    uint32_t nameOff;   // 4
    uint16_t off;       // 2
    int8_t   base;      // 1
    uint8_t  size;      // 1
    uint8_t  isWrite;   // 1
    uint8_t  pad[3];    // 3  ← C 有填充
} zh_rec_fieldacc;
```

**C 是 12 字节，Python 打包 10 字节 → 每条错位 2 字节。**

### 修复

```python
(5, 'fieldacc', '<IHBBBxxx'),   # 显式 3 字节填充 = 12
```

**规则**：**`pack_db.py` 的格式串必须和 C 结构体 `sizeof` 完全一致。** 加静态断言：

```c
_Static_assert(sizeof(zh_rec_fieldacc) == 12, "fieldacc must be 12 B");
```

---

## 陷阱 13：`ii_common` 缺空指针保护 ★★

### 现象

**站点装了但回调是 NULL → 闪退在地址 0。**

### 原因

```asm
ii_common:
    ...
    bl   ii_dispatch       ; 返回 x0 = 回调指针
    cbz  x0, ???           ; ← 早期版本没这个判断
    blr  x0                ; 空指针 → 崩
```

### 修复

```asm
    cbz  x16, 9f           ; x16 = 回调指针
    blr  x16
9:  ret                    ; 空就直接返回
```

---

## 陷阱 14：重定向的目标为 NULL ★★

### 现象

`zh_redirect` 装了，但回调返回 NULL 时崩。

### 原因

**`ii_redirect_dispatch` 早期版本返回 NULL**（表示"没改"），但 `ii_redirect_common` 直接 `br x16` → 跳到 0。

### 修复

```c
uint64_t ii_redirect_dispatch(ii_ctx* c, uint64_t sid) {
    ...
    uint64_t target = c->x[0];   // 回调返回
    if (!target) target = c->pc; // ★ NULL = 用原目标
    return target;
}
```

**并且 `ii_redirect_common` 里也加了 `cbz` 兜底。**

---

## 陷阱 15：`SHELL_SIZE` 太小 ★★

### 现象

**条件分支（`b.cond`）模式的 hook 一装就崩。**

### 原因

```
条件分支 trampoline 需要 44 字节
从 slot+32 开始 → 到 slot+76
SHELL_SIZE 只有 64 → 越过 12 字节
```

**踩到了下一个槽位的 shell。**

### 修复

```c
#define SHELL_SIZE 128   // 从 64 改成 128
```

---

## 陷阱 16：只做静态验证就上真机 ★★

### 现象

**静态检查全过，真机一跑就崩。**

### 原因

**静态检查看不出运行时问题**：
- 池是否被系统回收
- 线程是否处于可暂停状态
- 回调是否在热路径上阻塞

### 修复

**必须两步都做：**

1. 静态：`python3 _pyverify_all.py`
2. 真机：t01 → t13 逐步验证

**而且真机验证要一个一个来**：装一个 → 观察 → 卸载 → 装下一个。

---

## 陷阱 17：`next_n_clean` 太严格 ★

### 现象

**大多数函数都装不上，返回 `-7`。**

### 原因

```c
// 早期：要求后续 32 字节全部"干净"
if (!next_n_clean(addr, 32)) return ZH_E_WIDE;
```

**`next_n_clean` 拒绝分支指令和 `pc` 相对指令。32 字节 = 8 条指令，几乎必然包含至少一条分支。**

### 修复

```c
// 只要求前 4 条指令干净（16 字节模式搬走的量）
if (!next_n_clean(addr, 4)) return ZH_E_WIDE;
```

---

## 陷阱 18：`ii_engine_uninstall` 卸载全站点 ★★

### 现象

**卸载模块 A 的 hook，模块 B 的 hook 也一起没了。**

### 原因

**早期 `ii_engine_uninstall` 不管 owner，直接释放整个站点。**

### 修复

**给 `ii_cb_node` 加 `owner` 字段：**

```c
typedef struct ii_cb_node {
    ii_callback cb;
    void* user;
    int priority;
    int alive;
    int owner;              // ★ 新增
    struct ii_cb_node* next;
} ii_cb_node;
```

**卸载时只删本 owner 的节点：**

```c
for (ii_cb_node* n = s->cbs; n; n = n->next) {
    if (n->owner != myOwner) continue;
    n->alive = 0;
}
```

---

## 陷阱 19：日志打太多 ★

### 现象

**游戏卡顿，日志文件几分钟几百 MB。**

### 原因

**20 TPS 函数里每帧打一行：**

```c
static void on_tick(ii_ctx* c, void* user) {
    LOG_I(LOG_HOOK, "tick");   // 20 行/秒 = 1200 行/分钟
}
```

**× 10 个 hook = 12000 行/分钟 ≈ 1.2 MB/分钟。**

### 修复

**只在异常时打，或用原子计数器 + 定期汇总：**

```c
static volatile unsigned long g_n = 0;

static void on_tick(ii_ctx* c, void* user) {
    (void)c; (void)user;
    __atomic_add_fetch(&g_n, 1, __ATOMIC_RELAXED);   // 不打日志
}

// 每 5 秒汇总一次
```

---

## 陷阱 20：`zh_find` 返回值没检查 ★

### 现象

**`ii_install(0, cb, NULL)` → 返回 `-1`，但你没看 → 以为装上了。**

### 原因

```c
zh_hook(zh_find("不存在的函数"), cb, NULL);   // 传了 0
```

### 修复

```c
unsigned long addr = zh_find(fn);
if (!addr) {
    LOG_E(LOG_HOOK, "%s 不在数据表", fn);
    return;
}
zh_hook(fn, cb, NULL);
```

---

## 陷阱 21：多指手势期间 hook 阻塞 ★

### 现象

**注入后立刻玩家点屏幕 → 卡住 → ANR。**

### 原因

**回调阻塞在游戏主线程上，而主线程正在处理触摸事件。**

### 修复

**同陷阱 2** —— 回调不能阻塞。

---

## 陷阱 22：忘写 `destructor` ★★★

### 现象

**同一个进程反复加载/卸载你的库 → 第一次正常，第二次崩。**

### 原因

**第一次卸载时站点残留，第二次加载时 `zhreg` 里的池记录还指向旧地址。**

### 修复

```c
__attribute__((destructor))
static void cleanup(void) {
    zh_unhook_owner("my_mod");
}
```

**并且 `constructor` 里检查 `zh_layer_ready()`（可能已经初始化过）。**

---

## 陷阱 23：假设单线程 ★

### 现象

**偶发崩溃，重跑就不出现。**

### 原因

```c
static void* g_this = NULL;   // ★ 非线程安全

static void on_entry(ii_ctx* c, void* user) {
    g_this = (void*)c->x[0];
}
```

**游戏有多个线程调用同一个函数 → `g_this` 被覆盖。**

### 修复

```c
static __thread void* t_this = NULL;   // 线程局部
```

**或者用 `__atomic`：**

```c
static _Atomic(void*) g_this = NULL;
__atomic_store_n(&g_this, (void*)c->x[0], __ATOMIC_RELAXED);
```

---

## 陷阱 24：位宽写错 ★★

### 现象

**改了返回值但调用者拿到垃圾。**

### 原因

```c
void zh_set_ret_i32(ii_ctx* c, int32_t v) {
    c->x[0] = (uint64_t)v;   // ★ int32 负数会符号扩展到全 64 位
}
```

**如果调用者只读低 32 位，没问题；但如果函数实际返回 `int64`，高位就错了。**

### 修复

```c
void zh_set_ret_i32(ii_ctx* c, int32_t v) {
    c->x[0] = (uint64_t)(uint32_t)v;   // ★ 先转无符号再扩展
}
```

---

## 陷阱 25：浮点返回用错寄存器 ★★

### 现象

**改了 `zh_set_ret_float`，但返回值没变。**

### 原因

**`float`/`double` 返回值在 `q0`/`d0`，不是 `x0`。**

```c
c->x[0] = (uint64_t)1.0f;   // ★ 错，这是往整数寄存器写
```

### 修复

```c
void zh_set_ret_float(ii_ctx* c, float v) {
    memcpy(c->q[0], &v, 4);   // ★ 正确
}
```

---

## 三铁律速查

| 铁律 | 说明 |
|---|---|
| **1. 回调不阻塞** | 50 ms 告警，5 秒 ANR |
| **2. `constructor` 不 sleep** | 会冻结注入线程 |
| **3. 调试期一次只装一个库** | 为了定位问题，不是框架限制 |

---

## 下一步

- [FAQ](90-FAQ)
- [错误码](40-Reference-Error-Codes)
- [性能](41-Reference-Performance)
