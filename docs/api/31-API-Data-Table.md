# API：数据表

31,407 条记录，覆盖函数、字段、调用、常量、栈槽位。

---

## 一、表的分类

| 表名 | 记录数 | 内容 |
|---|---|---|
| `rettype` | 124 | 函数返回类型 |
| `retfunc` | 124 | 可 hook 的函数名（可回退） |
| `blcall` | 449 | `bl` 调用关系 |
| `const` | 26,447 | 函数内常量指令 |
| `constfunc` | 121 | 常量所属函数 |
| `frame` | 120 | 函数栈帧大小 |
| `slot` | 547 | 栈局部变量槽位 |
| `fieldacc` | 716 | 字段访问指令 |
| `poolhint` | 5 | trampoline 池位置 |
| `retpoint` | 2,754 | 函数返回点 |
| **合计** | **31,407** | |

---

## 二、通用接口

### 2.1 `zh_tbl_count()`

```c
int zh_tbl_count(int which);
```

**返回**：某表的记录数。

```c
LOG_I(LOG_HOOK, "rettype: %d", zh_tbl_count(ZH_TBL_RETTYPE));   // 124
LOG_I(LOG_HOOK, "const:   %d", zh_tbl_count(ZH_TBL_CONST));     // 26447
```

### 2.2 表 ID 常量

```c
enum {
    ZH_TBL_RETTYPE = 0,
    ZH_TBL_BLCALL,
    ZH_TBL_CONST,
    ZH_TBL_FRAME,
    ZH_TBL_SLOT,
    ZH_TBL_FIELDACC,
    ZH_TBL_CONSTFUNC,
    ZH_TBL_POOLHINT,
    ZH_TBL_RETFUNC,
    ZH_TBL_RETPOINT,
    ZH_TBL_MAX
};
```

---

## 三、rettype：返回类型

```c
int zh_tbl_rettype(int i, const char** funcName, uint32_t* off, int* kind, const char** typeName);
```

**字段**：

| 字段 | 含义 |
|---|---|
| `funcName` | 函数名 |
| `off` | 相对模块基址的偏移 |
| `kind` | 类型枚举 |
| `typeName` | 类型名字符串 |

**`kind` 枚举**：

| kind | 类型 |
|---|---|
| 0 | `void` |
| 1 | `int64` / 指针 |
| 2 | `int32` |
| 3 | `bool` |
| 4 | `float` |
| 5 | `double` |
| 6 | `ptr` |

**示例**：

```c
for (int i = 0; i < zh_tbl_count(ZH_TBL_RETTYPE); i++) {
    const char* name; uint32_t off; int kind; const char* tn;
    if (zh_tbl_rettype(i, &name, &off, &kind, &tn) != 0) continue;
    LOG_I(LOG_HOOK, "%-42s +0x%-8x %-8s (kind=%d)", name, off, tn, kind);
}
```

**输出**：
```
Actor_getHealth                            +0x5f2a1b0 float    (kind=4)
ItemStack_isNull                           +0x7d795008 bool    (kind=3)
LocalPlayer_normalTick                     +0x5c3e284 void    (kind=0)
...
```

---

## 四、retfunc：函数名列表

```c
const char* zh_tbl_retfunc(int i);
```

**作用**：枚举所有函数名（124 个）。

```c
for (int i = 0; i < zh_tbl_count(ZH_TBL_RETFUNC); i++) {
    const char* n = zh_tbl_retfunc(i);
    if (n) LOG_I(LOG_HOOK, "  [%3d] %s", i, n);
}
```

---

## 五、blcall：调用关系

```c
int zh_tbl_blcall(int i, const char** fromName, uint32_t* fromOff,
                        const char** toName,   uint32_t* toOff);
```

**作用**：每条记录是一个 `bl` 指令（谁调了谁）。

```c
for (int i = 0; i < zh_tbl_count(ZH_TBL_BLCALL); i++) {
    const char *f, *t; uint32_t fo, to;
    if (zh_tbl_blcall(i, &f, &fo, &t, &to) != 0) continue;
    LOG_I(LOG_HOOK, "%-40s +0x%-5x → %-40s +0x%x", f, fo, t, to);
}
```

**用途**：查某函数调了哪些函数 → 找到 `@Redirect` 的目标。

---

## 六、const：常量指令

```c
int zh_tbl_const(int i, int* funcIdx, int* kind, int* width,
                 uint32_t* insnOff, uint32_t* value);
```

| 字段 | 含义 |
|---|---|
| `funcIdx` | `constfunc` 表索引 |
| `kind` | 0=cmp 1=mov 2=addsub |
| `width` | 1/2/4/8 |
| `insnOff` | 相对函数入口 |
| `value` | 常量值 |

**配合 `constfunc`**：

```c
int nfunc = zh_tbl_count(ZH_TBL_CONSTFUNC);
for (int i = 0; i < zh_tbl_count(ZH_TBL_CONST); i++) {
    int fi, kind, width; uint32_t io, v;
    if (zh_tbl_const(i, &fi, &kind, &width, &io, &v) != 0) continue;
    if (fi < 0 || fi >= nfunc) continue;

    const char* fname = zh_tbl_constfunc(fi);
    LOG_I(LOG_HOOK, "%s +0x%x value=%u kind=%d w=%d",
          fname, io, v, kind, width);
}
```

---

## 七、constfunc：常量所属函数

```c
const char* zh_tbl_constfunc(int i);
```

**作用**：`const` 表里的 `funcIdx` 索引到这里。

---

## 八、frame：栈帧

```c
int zh_tbl_frame(const char* funcName, int* frameSize);
```

**或者按索引**：

```c
int zh_tbl_frame_at(int i, const char** funcName, int* frameSize);
```

**示例**：

```c
int fs = 0;
if (zh_tbl_frame("ItemStack_isNull", &fs) == 0) {
    LOG_I(LOG_LOCAL, "栈帧 0x%x", fs);   // 0x440
}
```

---

## 九、slot：栈槽位

```c
int zh_tbl_slot(int i, int* funcIdx, uint16_t* spOff,
                uint8_t* size, uint32_t* insnOff);
```

| 字段 | 含义 |
|---|---|
| `funcIdx` | 所属函数 |
| `spOff` | 相对 sp 偏移 |
| `size` | 宽度 |
| `insnOff` | 首次访问指令 |

---

## 十、fieldacc：字段访问

```c
int zh_tbl_fieldacc(int i, const char** funcName, uint16_t* off,
                    int* base, int* size, int* isWrite);
```

| 字段 | 含义 |
|---|---|
| `funcName` | 函数名（通常是简单 getter） |
| `off` | 字段偏移 |
| `base` | 基址寄存器（0=`x0`, 1=`x1`, ...） |
| `size` | 宽度 |
| `isWrite` | 1 = 写，0 = 读 |

**示例**：

```c
for (int i = 0; i < zh_tbl_count(ZH_TBL_FIELDACC); i++) {
    const char* fn; uint16_t off; int base, size, wr;
    if (zh_tbl_fieldacc(i, &fn, &off, &base, &size, &wr) != 0) continue;
    LOG_I(LOG_HOOK, "%-40s +0x%-5x x%d/%d %s",
          fn, off, base, size, wr ? "写" : "读");
}
```

---

## 十一、poolhint：trampoline 池

```c
int zh_tbl_poolhint(int i, uint32_t* vaddr, uint32_t* size);
```

**5 条记录**：

| # | 大小 |
|---|---|
| 0 | 308 KB |
| 1 | 244 KB |
| 2 | 20 KB |
| 3 | 8 KB |
| 4 | 7 KB |

**`vaddr`** 是相对模块基址的偏移，引擎启动时转成绝对地址。

---

## 十二、retpoint：返回点

```c
int zh_tbl_retpoint(int i, uint32_t* funcIdx, uint32_t* insnOff);
```

| 字段 | 含义 |
|---|---|
| `funcIdx` | 所属函数 |
| `insnOff` | 相对函数入口 |

**2,754 条记录，平均每函数 22 个。**

---

## 十三、按名字查记录

### 13.1 `ii_lookup_*` 系列

```c
int ii_lookup_func(const char* name, unsigned long* outAddr);
int ii_lookup_frame(const char* name, int* outFrameSize);
int ii_lookup_field(const char* name, uint16_t* outOff, int* outBase,
                    int* outSize, int* outIsWrite);
int ii_lookup_const(const char* name, uint32_t value, int nth, unsigned long* outAddr);
```

**这些是封装好的高层接口，直接给绝对地址。**

---

## 十四、运行时加载

### 14.1 静态表（默认）

**编译进 `.so` 的 `zh_db_full.bin`（367 KB）。**

**优点**：无外部依赖
**缺点**：`.so` 大

### 14.2 运行时文件

```c
int zh_tbl_load(const char* path);
int zh_tbl_loaded(void);
```

**从文件加载最新表**（可在游戏版本更新后只换表，不重编 `.so`）。

```c
if (zh_tbl_load("/sdcard/Download/zh_db_new.bin") == 0) {
    LOG_I(LOG_HOOK, "已加载新表: %d 条", zh_tbl_total());
}
```

**格式**：见 [数据格式](42-Reference-Data-Format)。

---

## 十五、表校验

```c
int zh_db_verify(void);
```

**作用**：检查表完整性（记录数、偏移范围）。

**返回**：0 = 正常，负数 = 有问题。

```c
int rc = zh_db_verify();
if (rc != 0) {
    LOG_E(LOG_HOOK, "数据表校验失败 rc=%d", rc);
}
```

**校验项**：
- 表头 magic
- 每表记录数
- 偏移是否在 `.so` 范围内
- 函数名是否可打印

---

## 十六、统计信息

```c
int zh_tbl_total(void);       // 总记录数
```

```c
LOG_I(LOG_HOOK, "数据表 %d 条", zh_tbl_total());   // 31407
```

---

## 十七、完整示例：导出全部数据

```c
#include <stdio.h>
#include <unistd.h>
#include "zh/zh_layer.h"
#include "zh/zh_tbl.h"
#include "zh/zlog.h"

static void dump_all(void) {
    unsigned long base = zh_module_base();

    LOG_I(LOG_HOOK, "════════ 数据表 %d 条 ════════", zh_tbl_total());

    // 1. rettype
    LOG_I(LOG_HOOK, "── 返回类型 %d 条 ──", zh_tbl_count(ZH_TBL_RETTYPE));
    for (int i = 0; i < zh_tbl_count(ZH_TBL_RETTYPE); i++) {
        const char* n; uint32_t o; int k; const char* tn;
        if (zh_tbl_rettype(i, &n, &o, &k, &tn) != 0) continue;
        LOG_I(LOG_HOOK, "  %-40s +0x%-8x %s", n, o, tn);
    }

    // 2. fieldacc（只打前 30 条）
    LOG_I(LOG_HOOK, "── 字段访问 %d 条（前 30）──",
          zh_tbl_count(ZH_TBL_FIELDACC));
    for (int i = 0, c = 0; i < zh_tbl_count(ZH_TBL_FIELDACC) && c < 30; i++, c++) {
        const char* f; uint16_t o; int b, s, w;
        if (zh_tbl_fieldacc(i, &f, &o, &b, &s, &w) != 0) continue;
        LOG_I(LOG_HOOK, "  %-40s +0x%-4x x%d/%d %s", f, o, b, s, w ? "W" : "R");
    }

    // 3. blcall（只打前 20 条）
    LOG_I(LOG_HOOK, "── 调用关系 %d 条（前 20）──",
          zh_tbl_count(ZH_TBL_BLCALL));
    for (int i = 0, c = 0; i < zh_tbl_count(ZH_TBL_BLCALL) && c < 20; i++, c++) {
        const char *fr, *to; uint32_t fo, too;
        if (zh_tbl_blcall(i, &fr, &fo, &to, &too) != 0) continue;
        LOG_I(LOG_HOOK, "  %-30s +0x%-5x → %-30s +0x%x", fr, fo, to, too);
    }

    // 4. frame
    LOG_I(LOG_HOOK, "── 栈帧 %d 条 ──", zh_tbl_count(ZH_TBL_FRAME));
    for (int i = 0; i < zh_tbl_count(ZH_TBL_FRAME); i++) {
        const char* n; int fs;
        if (zh_tbl_frame_at(i, &n, &fs) != 0) continue;
        LOG_I(LOG_HOOK, "  %-40s 0x%x", n, fs);
    }

    LOG_I(LOG_HOOK, "════════ 结束 (base=0x%lx) ════════", base);
}

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;
    dump_all();
    while (1) sleep(10);
    return NULL;
}

__attribute__((constructor))
static void run(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}
```

---

## 下一步

- [数据格式](42-Reference-Data-Format)
- [函数查找](21-API-Find-Function)
- [偏移采集指南](50-Guide-Offset-Collection)
