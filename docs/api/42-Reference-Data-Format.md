# 参考：数据格式

`zh_db_full.bin` 的二进制布局。

---

## 一、文件总览

```
zh_db_full.bin  (367,193 字节)
├─ 文件头        32 字节
├─ 表索引        10 × 24 = 240 字节
├─ 字符串池      变长
└─ 表数据        变长
```

---

## 二、文件头（32 字节）

```c
#define ZH_DB_MAGIC   0x5A484442u   // "ZHDB"
#define ZH_DB_VERSION 2u

typedef struct {
    uint32_t magic;      // 0x5A484442
    uint32_t version;    // 2
    uint32_t nTables;    // 10
    uint32_t hdrLen;     // 32
    uint32_t strOff;     // 字符串池偏移
    uint32_t strLen;     // 字符串池长度
    uint8_t  sha256[8];  // 前 8 字节校验
    uint32_t total;      // 总记录数
} zh_db_hdr;
```

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0x00 | `magic` | 必须 `0x5A484442` |
| 0x04 | `version` | 目前 `2` |
| 0x08 | `nTables` | `10` |
| 0x0C | `hdrLen` | `32` |
| 0x10 | `strOff` | 字符串池起始 |
| 0x14 | `strLen` | 字符串池长度 |
| 0x18 | `sha256[0..7]` | 校验 |
| 0x20 | `total` | 31,407 |

**最小文件长度**：`hdrLen` (32) + `nTables × 24` (240) = **272 字节**。

**`test_db` 的教训**：早期测试用 12 字节文件 → 小于 `HDR_LEN` → 读越界。现在校验：

```c
if (fileLen < HDR_LEN) return ZH_E_ARG;
```

---

## 三、表索引（每表 24 字节）

```c
typedef struct {
    uint32_t id;       // 表 ID（0..9）
    uint32_t count;    // 记录数
    uint32_t recSize;  // 单条记录字节数
    uint32_t dataOff;  // 数据起始偏移
    uint32_t dataLen;  // 数据长度 = count × recSize
    uint32_t crc;      // 简单校验
} zh_db_tbl;
```

| 偏移 | 字段 |
|---|---|
| +0x00 | `id` |
| +0x04 | `count` |
| +0x08 | `recSize` |
| +0x0C | `dataOff` |
| +0x10 | `dataLen` |
| +0x14 | `crc` |

---

## 四、十张表的布局

### 4.1 `rettype`（id = 0）

```c
// recSize = 12
typedef struct {
    uint32_t nameOff;   // 字符串池偏移（函数名）
    uint32_t insnOff;   // 相对模块基址的偏移
    uint16_t kind;      // 返回类型枚举
    uint16_t typeOff;   // 字符串池偏移（类型名）
    uint16_t pad;
} __attribute__((packed)) zh_rec_rettype;
```

**实际 `<IIHHxx>` = 12 字节。**

**124 条。**

---

### 4.2 `blcall`（id = 1）

```c
// recSize = 12
typedef struct {
    uint32_t fromOff;   // 调用者名字串偏移
    uint32_t fromInsn;  // 调用指令偏移
    uint32_t toOff;     // 目标名字串偏移
} __attribute__((packed)) zh_rec_blcall;
```

**`<III>` = 12 字节。449 条。**

**注意**：没有 `toInsn`，目标偏移从 `rettype` 表查。

---

### 4.3 `const`（id = 2）

```c
// recSize = 12
typedef struct {
    uint16_t funcIdx;   // constfunc 表索引
    uint8_t  kind;      // 0=cmp 1=mov 2=addsub
    uint8_t  width;     // 1/2/4/8
    uint32_t insnOff;   // 相对函数入口
    uint32_t value;     // 常量值
} __attribute__((packed)) zh_rec_const;
```

**`<HBBII>` = 12 字节。26,447 条 —— 最大的表。**

---

### 4.4 `frame`（id = 3）

```c
// recSize = 8
typedef struct {
    uint32_t nameOff;   // 函数名
    uint32_t frameSize; // 栈帧字节数
} __attribute__((packed)) zh_rec_frame;
```

**`<II>` = 8 字节。120 条。**

---

### 4.5 `slot`（id = 4）

```c
// recSize = 12
typedef struct {
    uint32_t funcIdx;   // frame 表索引（或函数名字串偏移）
    uint16_t spOff;     // 相对 sp 偏移
    uint8_t  size;      // 1/2/4/8
    uint8_t  pad;
    uint32_t insnOff;   // 首次访问指令
} __attribute__((packed)) zh_rec_slot;
```

**`<IHBBI>` = 12 字节。547 条。**

---

### 4.6 `fieldacc`（id = 5）

```c
// recSize = 12  ★ 曾经是 10，导致整表错位
typedef struct {
    uint32_t nameOff;   // 函数名
    uint16_t off;       // 字段偏移
    int8_t   base;      // 基址寄存器索引
    uint8_t  size;      // 宽度
    uint8_t  isWrite;   // 1 = 写
    uint8_t  pad[3];    // ← 关键填充
} __attribute__((packed)) zh_rec_fieldacc;
```

**`<IHBBBxxx>` = 12 字节。716 条。**

> **历史 bug**：`pack_db.py` 用 `<IHBBBB`（10 字节），但 C 结构体声明为 12 字节 → **每条记录错位 2 字节，整表报废**。
>
> 修复：改用 `<IHBBBxxx`（显式 3 字节填充）。

---

### 4.7 `constfunc`（id = 6）

```c
// recSize = 8
typedef struct {
    uint32_t nameOff;   // 函数名
    uint32_t insnOff;   // 函数入口偏移
} __attribute__((packed)) zh_rec_constfunc;
```

**`<II>` = 8 字节。121 条。**

**为什么 `const` 是 26,447 但 `constfunc` 只有 121**：大部分函数没有常量记录，只有 121 个函数被扫描到常量。

---

### 4.8 `poolhint`（id = 7）

```c
// recSize = 8
typedef struct {
    uint32_t vaddr;     // 相对模块基址
    uint32_t size;      // 池大小
} __attribute__((packed)) zh_rec_poolhint;
```

**`<II>` = 8 字节。5 条。**

---

### 4.9 `retfunc`（id = 8）

```c
// recSize = 4
typedef struct {
    uint32_t nameOff;   // 函数名
} __attribute__((packed)) zh_rec_retfunc;
```

**`<I>` = 4 字节。124 条。**

**纯名字列表**（124 个可 hook 函数）。

---

### 4.10 `retpoint`（id = 9）

```c
// recSize = 8
typedef struct {
    uint32_t funcIdx;   // retfunc 表索引
    uint32_t insnOff;   // 相对函数入口
} __attribute__((packed)) zh_rec_retpoint;
```

**`<II>` = 8 字节。2,754 条。**

---

## 五、字符串池

**所有名字串连续存放，用 `\0` 分隔。**

```
偏移 0x000: "Actor_getHealth\0"
偏移 0x010: "ItemStack_isNull\0"
...
```

**读取**：

```c
static const char* str_at(const void* strPool, uint32_t off) {
    return (const char*)strPool + off;
}
```

**无长度前缀** —— 直接 `strlen` 即可。

---

## 六、字节序

**小端（ARM64 / x86_64 原生）。**

`pack_db.py` 用 `struct.pack("<...")`。

---

## 七、校验

### 7.1 加载时校验

```c
int zh_tbl_load(const char* path) {
    // 1. magic
    if (hdr->magic != ZH_DB_MAGIC) return ZH_E_ARG;

    // 2. version
    if (hdr->version != ZH_DB_VERSION) return ZH_E_ARG;

    // 3. 文件长度
    if (fileLen < hdr->hdrLen + hdr->nTables * 24) return ZH_E_ARG;

    // 4. 每表范围
    for (int i = 0; i < hdr->nTables; ++i) {
        zh_db_tbl* t = &tbls[i];
        if (t->dataOff + t->dataLen > fileLen) return ZH_E_ARG;
        if (t->dataLen != t->count * t->recSize) return ZH_E_ARG;
    }

    return ZH_OK;
}
```

### 7.2 `verify_db.py` 独立校验

```python
def verify(path):
    data = open(path, 'rb').read()
    magic, ver, ntab, hdrlen, stroff, strlen_, = struct.unpack_from('<IIIIII', data, 0)
    assert magic == 0x5A484442, "magic"
    assert ver == 2, "version"

    total = 0
    for i in range(ntab):
        tid, cnt, recsz, doff, dlen, crc = struct.unpack_from('<IIIIII', data,
                                                               hdrlen + i*24)
        assert dlen == cnt * recsz, f"table {tid} size mismatch"
        assert doff + dlen <= len(data), f"table {tid} out of range"
        total += cnt

    print(f"OK  {ntab} 表, {total} 条, {len(data)} 字节")
```

**实测**：

```
$ python3 tools/verify_db.py data/zh_db_full.bin
OK  10 表, 31407 条, 367193 字节
```

---

## 八、生成流程

### 8.1 `pack_db.py`

```bash
python3 tools/pack_db.py <输入目录> -o data/zh_db_full.bin
```

**输入**：9 个 `.h` 头文件（`data/*_db.h`）。

**流程**：
1. 解析 `static const` 数组
2. 建字符串池（去重）
3. 排序表（按 ID）
4. 生成二进制
5. 写校验

### 8.2 关键代码片段

```python
def pack_table(records, fmt_rec):
    out = bytearray()
    for r in records:
        out += struct.pack(fmt_rec, *r)
    return bytes(out)

TABLES = [
    (0, 'rettype',  '<IIHHxx'),
    (1, 'blcall',   '<III'),
    (2, 'const',    '<HBBII'),
    (3, 'frame',    '<II'),
    (4, 'slot',     '<IHBBI'),
    (5, 'fieldacc', '<IHBBBxxx'),   # ★ 填充 3 字节
    (6, 'constfunc','<II'),
    (7, 'poolhint', '<II'),
    (8, 'retfunc',  '<I'),
    (9, 'retpoint', '<II'),
]
```

---

## 九、表间引用

```
retfunc[i]        ← 名字
   ↑
   │ funcIdx
retpoint[funcIdx].insnOff

constfunc[i]      ← 名字
   ↑
   │ funcIdx
const[funcIdx].insnOff

frame[i]          ← 名字
   ↑
   │ funcIdx
slot[funcIdx].spOff

rettype[i]        ← 名字 + insnOff（独立，不需要索引）
blcall[i]         ← 两个名字（独立）
fieldacc[i]       ← 名字 + 偏移（独立）
poolhint[i]       ← 偏移 + 大小（独立）
```

**索引关系很浅** —— 只有 3 处需要解引用。

---

## 十、运行时修改

**加载后表在只读内存里（`mmap` 后 `mprotect(PROT_READ)`）。**

**要修改**：重新加载一个新表文件。

```c
zh_tbl_load("/sdcard/Download/zh_db_new.bin");
```

**注意**：已安装的站点用的是旧地址 —— 换表后要卸载重装。

---

## 十一、手工检查数据

```bash
# 看文件头
xxd -l 32 zh_db_full.bin

# 应该看到
# 00000000: 4244 485a 0200 0000 0a00 0000 2000 0000  BDHZ........ ...
#           ^^^^^^^^ = 0x5A484442 (小端)
```

**Python 快速看表**：

```python
import struct
d = open('zh_db_full.bin','rb').read()
h = struct.unpack_from('<IIIIII', d, 0)
print("magic=%08x ver=%d tables=%d" % (h[0], h[1], h[2]))
for i in range(h[2]):
    t = struct.unpack_from('<IIIIII', d, 32 + i*24)
    print("  table %d: %6d recs × %2d B @0x%06x" % (t[0], t[1], t[2], t[3]))
```

**输出**：

```
magic=5a484442 ver=2 tables=10
  table 0:    124 recs × 12 B @0x0002a8
  table 1:    449 recs × 12 B @0x000874
  table 2:  26447 recs × 12 B @0x001d94
  table 3:    120 recs ×  8 B @0x4e0a4
  table 4:    547 recs × 12 B @0x4e460
  table 5:    716 recs × 12 B @0x4f300
  table 6:    121 recs ×  8 B @0x50750
  table 7:      5 recs ×  8 B @0x50b18
  table 8:    124 recs ×  4 B @0x50b40
  table 9:   2754 recs ×  8 B @0x50d30
```

---

## 下一步

- [数据表查询](31-API-Data-Table)
- [偏移采集指南](50-Guide-Offset-Collection)
- [版本更新](51-Guide-Version-Update)
