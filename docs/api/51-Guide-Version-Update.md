# 指南：版本更新

游戏更新后要做什么。

---

## 一、更新会改变什么

**游戏每次更新，`.so` 重新编译 → 所有偏移全变。**

| 项 | 会变吗 |
|---|---|
| 函数地址 | ✅ 全变 |
| 字段偏移 | ✅ 常变（结构体改动） |
| 栈帧大小 | ✅ 常变 |
| 常量位置 | ✅ 全变 |
| 函数名 | ⚠️ 部分变（NetEase 偶尔改名） |
| 调用关系 | ⚠️ 部分变 |

**唯一不变的是函数名（如果 NetEase 不改）。**

---

## 二、更新流程

### 2.1 第一步：确认版本变化

```bash
# 旧版本记录
ls -l libminecraftpe_old.so   # 323,630,848
sha256sum libminecraftpe_old.so

# 新版本
adb pull /data/app/.../lib/arm64/libminecraftpe.so
ls -l libminecraftpe.so
sha256sum libminecraftpe.so
```

**如果大小和哈希都没变 → 不需要重采。**

### 2.2 第二步：引擎自动检测

**引擎启动时会算指纹：**

```c
uint64_t zh_game_version(void);   // 0 = 未知
```

**探针里加检查**：

```c
#define EXPECTED_VERSION 0xbfdc5d91cc459fb0ULL

static void* worker(void* arg) {
    (void)arg;
    if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) return NULL;

    uint64_t v = zh_game_version();
    if (v != EXPECTED_VERSION) {
        LOG_E(LOG_INIT, "游戏版本不匹配！");
        LOG_E(LOG_INIT, "  期望 0x%016llx", EXPECTED_VERSION);
        LOG_E(LOG_INIT, "  实际 0x%016llx", v);
        LOG_E(LOG_INIT, "  数据表可能全错，不要安装!");
        return NULL;
    }

    // 安全，继续
    ...
}
```

**这是最重要的一步** —— 版本不对就别装。

### 2.3 第三步：重采数据

**完整流程见 [偏移采集](50-Guide-Offset-Collection)。**

**快速版**：

```bash
# 1. 拉 .so
adb pull /data/app/.../lib/arm64/libminecraftpe.so

# 2. 重新提取所有表
python3 extract_sem.py --so libminecraftpe.so --names names.txt -o funcs.json
python3 scan_all.py --so libminecraftpe.so --funcs funcs.json --out data/

# 3. 打包 + 校验
python3 tools/pack_db.py data -o data/zh_db_full.bin
python3 tools/verify_db.py data/zh_db_full.bin
```

### 2.4 第四步：静态校验

```bash
python3 _pyverify_all.py
```

**必须全绿才继续。**

### 2.5 第五步：真机分级验证

**按 t01 → t13 顺序跑，每步通过再下一步：**

| 测试 | 验证什么 | 必须通过 |
|---|---|---|
| t01 | 初始化 + 基址 | ✅ |
| t02 | `zh_find` 124/124 | ✅ |
| t03 | hook / unhook | ✅ |
| t04 | LIGHT 20 TPS | ✅ |
| t05 | 返回点 | ✅ |
| t06 | 字段读写 | ✅ |
| t07 | 栈局部变量 | ✅ |
| t08 | 多回调优先级 | ✅ |
| t09 | 崩溃隔离 | ✅ |
| t10 | 数据表加载 | ✅ |
| t11 | 多实例 | ✅ |
| t12 | 重定向 | ✅ |
| t13 | 常量 | ✅ |

**任何一步失败就停下来查。**

---

## 三、函数名变了怎么办

### 3.1 症状

```c
unsigned long a = zh_find("LocalPlayer_normalTick");
if (!a) {
    // 名字不在了
}
```

### 3.2 找新名字

**方法 1：搜索字符串**

```python
import re
data = open('libminecraftpe.so','rb').read()
for m in re.finditer(rb'[A-Z][A-Za-z0-9_]{8,60}', data):
    s = m.group().decode('ascii', 'ignore')
    if 'NormalTick' in s or 'normalTick' in s:
        print(s)
```

**方法 2：按行为找**

**`normalTick` 的特征**：
- 20 次/秒被调用
- 调用 `Actor_tick`

**用调用关系反查**：

```python
# 找谁被 Level_tick 每秒调 20 次
callers = find_callers_of('Actor_tick')
for c in callers:
    print(c)
```

### 3.3 名字映射表

**建议维护一个映射：**

```c
typedef struct {
    const char* oldName;
    const char* newName;
} zh_name_map;

static const zh_name_map k_nameMap[] = {
    { "LocalPlayer_normalTick", "LocalPlayer_tick" },   // 示例
    { NULL, NULL }
};

unsigned long zh_find_compat(const char* name) {
    unsigned long a = zh_find(name);
    if (a) return a;

    for (int i = 0; k_nameMap[i].oldName; i++) {
        if (strcmp(name, k_nameMap[i].oldName) != 0) continue;
        a = zh_find(k_nameMap[i].newName);
        if (a) {
            LOG_W(LOG_TBL, "%s 已改名 %s，已兼容",
                  name, k_nameMap[i].newName);
            return a;
        }
    }
    return 0;
}
```

---

## 四、字段偏移变了怎么办

**这是最常见的破坏。**

### 4.1 症状

```
字段值看起来是垃圾（0xdeadbeef / 极大数 / 0）
游戏行为异常但不崩
```

### 4.2 诊断

```c
static void check_field(ii_ctx* c, const char* fn, uint16_t off, uint8_t size) {
    int64_t v = zh_field_get(c, 0, off, size);
    if (size == 8 && v != 0) {
        // 指针合法性检查
        if ((uint64_t)v < 0x10000 || (uint64_t)v > 0x800000000000ULL) {
            LOG_W(LOG_TBL, "%s +0x%x 值可疑: 0x%llx（偏移可能失效）",
                  fn, off, (unsigned long long)v);
        }
    }
}
```

### 4.3 重采

**`fieldacc` 靠"简单 getter 模式"自动识别。**

**如果游戏改了结构体布局，偏移自然更新。**

**但要验证语义没变**：

```python
# 旧版：Actor_getLevel → +0x250
# 新版：Actor_getLevel → +0x258  ← 偏移变了但语义一样，OK
# 新版：Actor_getLevel → 找不到 ← 函数被内联了，要换目标
```

---

## 五、兼容性设计

### 5.1 探测式安装

**不要硬依赖某个函数，而是按优先级试：**

```c
static int install_with_fallback(const char* const* names,
                                 ii_callback cb, void* user) {
    for (int i = 0; names[i]; i++) {
        if (!zh_find(names[i])) continue;

        int rc = zh_hook(names[i], cb, user);
        if (rc == 0) {
            LOG_I(LOG_HOOK, "✓ 用 %s", names[i]);
            return 0;
        }
        LOG_W(LOG_HOOK, "%s 存在但安装失败 rc=%d", names[i], rc);
    }

    LOG_E(LOG_HOOK, "✗ 所有候选都失败");
    return -1;
}

// 用法
static const char* k_tickFuncs[] = {
    "LocalPlayer_normalTick",
    "LocalPlayer_tick",
    "Player_tickWorld",
    NULL
};
install_with_fallback(k_tickFuncs, on_tick, NULL);
```

### 5.2 软降级

```c
typedef struct {
    int hasTick;
    int hasHealth;
    int hasField;
} capabilities_t;

static capabilities_t g_caps;

static void probe(void) {
    g_caps.hasTick   = zh_find("LocalPlayer_normalTick") != 0;
    g_caps.hasHealth = zh_find("Actor_getHealth") != 0;
    g_caps.hasField  = 1;   // 字段总是可用

    LOG_I(LOG_INIT, "能力: tick=%d health=%d",
          g_caps.hasTick, g_caps.hasHealth);
}

static void setup(void) {
    if (g_caps.hasTick)   zh_hook("LocalPlayer_normalTick", on_tick, NULL);
    if (g_caps.hasHealth) zh_hook_ret("Actor_getHealth", on_health, NULL);
}
```

**核心功能有，扩展功能没有 —— 总比全崩好。**

---

## 六、运行时换表

**不用重编 `.so`，只换数据文件：**

```c
if (zh_tbl_load("/sdcard/Download/zh_db_new.bin") == 0) {
    LOG_I(LOG_TBL, "✓ 已加载新表 %d 条", zh_tbl_total());

    // ★ 必须：卸载所有旧的（旧表地址已失效）
    zh_unhook_all();

    // 按新表重装
    setup();
}
```

**限制**：`zh_tbl_load` 必须在**没有已安装站点**时调用，否则旧站点的地址在新表里找不到。

**更安全的做法**：只在初始化阶段换表。

---

## 七、多版本支持

### 7.1 一个 `.so` 支持多个游戏版本

**嵌入多份数据表：**

```c
// 版本 A 的表
static const unsigned char k_db_v1[] = { ... };
// 版本 B 的表
static const unsigned char k_db_v2[] = { ... };

struct { uint64_t version; const void* data; size_t len; } k_dbs[] = {
    { 0xbfdc5d91cc459fb0ULL, k_db_v1, sizeof k_db_v1 },
    { 0x1a2b3c4d5e6f7890ULL, k_db_v2, sizeof k_db_v2 },
};

int init_for_version(uint64_t ver) {
    for (int i = 0; i < 2; i++) {
        if (k_dbs[i].version != ver) continue;
        return zh_tbl_load_mem(k_dbs[i].data, k_dbs[i].len);
    }
    return ZH_E_ARG;
}
```

**代价**：`.so` 变大（每份表 367 KB）。

**取舍**：支持 2 个版本 +734 KB，可接受。

### 7.2 外部数据 + 版本路由

**更省空间的做法：数据表放外部，按版本选文件。**

```c
char path[256];
snprintf(path, sizeof path,
         "/data/data/com.netease.x19/files/zhprobe/db_%016llx.bin",
         zh_game_version());

if (zh_tbl_load(path) != 0) {
    LOG_E(LOG_TBL, "没有 %s", path);
    return NULL;
}
```

---

## 八、更新检查清单

```
□ 1. 拉新 .so，记录大小 + sha256
□ 2. 重跑 extract_sem.py，确认 124 个函数都找到
□ 3. 重跑 scan_all.py，生成 9 个 .h
□ 4. pack_db.py 打包
□ 5. verify_db.py 校验（记录数对不对）
□ 6. _pyverify_all.py 静态全量校验
□ 7. 更新 EXPECTED_VERSION 常量
□ 8. 重编 libzh_engine.so
□ 9. 真机跑 t01 → t13，逐步验证
□ 10. 记录失败项，针对性修
```

---

## 九、常见失败模式

| 现象 | 原因 | 修法 |
|---|---|---|
| `zh_find` 全失败 | 表没加载 / 版本不对 | 查 `zh_tbl_total()` |
| t02 有失败项 | 个别函数重采失败 | 单独查那个函数 |
| t03 安装失败 `-5` | 函数序言变了 | 换函数或返回点 |
| t05 返回点装不上 | `retpoint` 表过期 | 重采 `retpoint` |
| t06 读到垃圾 | 字段偏移变了 | 重采 `fieldacc` |
| t07 栈帧 0 | `frame` 表没覆盖该函数 | 重采 `frame` |
| t11 key 不匹配 | 结构体布局变了 | 重采 `fieldacc` |
| t12 调用点找不到 | `blcall` 表过期 | 重采 `blcall` |
| t13 常量找不到 | `const` 表过期 | 重采 `const` |

---

## 十、自动化脚本

**一键更新脚本：**

```bash
#!/bin/sh
set -e

SO=$1
OUT=data

echo "=== 1. 提取函数 ==="
python3 extract_sem.py --so "$SO" --names names.txt -o funcs.json

echo "=== 2. 扫描各表 ==="
python3 scan_all.py --so "$SO" --funcs funcs.json --out "$OUT"

echo "=== 3. 打包 ==="
python3 tools/pack_db.py "$OUT" -o "$OUT/zh_db_full.bin"

echo "=== 4. 校验 ==="
python3 tools/verify_db.py "$OUT/zh_db_full.bin"

echo "=== 5. 静态全量审计 ==="
python3 _pyverify_all.py

echo "=== 完成 ==="
echo "记得更新 EXPECTED_VERSION 并重编 .so"
```

---

## 下一步

- [偏移采集](50-Guide-Offset-Collection)
- [常见陷阱](52-Guide-Pitfalls)
- [FAQ](90-FAQ)
