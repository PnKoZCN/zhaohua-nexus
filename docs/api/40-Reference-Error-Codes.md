# 参考：错误码

所有返回码、含义、排查方向。

---

## 一、错误码总表

```c
#define ZH_OK           0
#define ZH_E_ARG       -1
#define ZH_E_DUP       -2
#define ZH_E_FULL      -3
#define ZH_E_ENGINE    -4
#define ZH_E_BADINSN   -5
#define ZH_E_NOMEM     -6
#define ZH_E_WIDE      -7
#define ZH_E_ALLOC     -9
#define ZH_E_PATCH    -10
#define ZH_E_REACH    -11
#define ZH_E_TAIL     -13
#define ZH_E_SHELL    -15
#define ZH_E_NOMULTI  -16
#define ZH_E_SUSPEND  -17
```

| 码 | 名称 | 含义 |
|---|---|---|
| 0 | `ZH_OK` | 成功 |
| -1 | `ZH_E_ARG` | 参数无效 |
| -2 | `ZH_E_DUP` | 已存在站点 |
| -3 | `ZH_E_FULL` | 表已满 |
| -4 | `ZH_E_ENGINE` | 引擎未初始化 |
| -5 | `ZH_E_BADINSN` | 指令不可插桩 |
| -6 | `ZH_E_NOMEM` | 内存不足 |
| -7 | `ZH_E_WIDE` | 无法覆盖 16 字节 |
| -9 | `ZH_E_ALLOC` | 分配失败 |
| -10 | `ZH_E_PATCH` | 写指令失败 |
| -11 | `ZH_E_REACH` | 跳转超范围 |
| -13 | `ZH_E_TAIL` | 后续指令不干净 |
| -15 | `ZH_E_SHELL` | shell 分配失败 |
| -16 | `ZH_E_NOMULTI` | 不支持多回调 |
| -17 | `ZH_E_SUSPEND` | 暂停线程失败 |

---

## 二、逐个详解

### -1 `ZH_E_ARG` 参数无效

**触发**：
- `addr == 0`
- `cb == NULL`
- `funcName == NULL`
- 函数名不在数据表（`zh_hook` 返回这个）

**排查**：

```c
if (!addr)  LOG_E(LOG_HOOK, "地址为 0");
if (!cb)    LOG_E(LOG_HOOK, "回调为 NULL");
if (!zh_find(fn)) LOG_E(LOG_HOOK, "函数 %s 不在数据表", fn);
```

**修复**：
```c
unsigned long addr = zh_find(fn);
if (!addr) return;              // 先确认存在
if (!cb) return;                // 先确认回调有效
```

---

### -2 `ZH_E_DUP` 已存在站点

**触发**：该地址已经装了站点，再调 `ii_install`。

**解释**：一个地址只能有一个站点（但站点内可以有多个回调）。

**修复**：

```c
int rc = zh_hook(fn, cb, user);
if (rc == -2) {
    // 追加到已有的链
    ii_add_callback(zh_find(fn), cb, user, 0);
}
```

---

### -3 `ZH_E_FULL` 表已满

**触发**：
- 站点数达上限
- 多实例槽位满

**实际上限**：
- 理论：262,144
- `sid` 编码限制：65,535
- shell 池限制：**约 4,700**

**修复**：

```c
if (rc == -3) {
    // 卸载不用的
    zh_unhook("不常用的函数");
    // 或全清
    zh_unhook_all();
    // 重试
}
```

---

### -4 `ZH_E_ENGINE` 引擎未初始化

**触发**：没调 `zh_layer_init*` 就安装。

**修复**：

```c
if (zh_layer_init_default() != ZH_OK && !zh_layer_ready()) {
    LOG_E(LOG_INIT, "引擎初始化失败");
    return NULL;
}
// 现在可以装了
```

---

### -5 `ZH_E_BADINSN` 指令不可插桩

**触发**：函数开头有特殊指令：
- `pc` 相对寻址（`adr`/`adrp`）
- 遗留 IT 块
- 加密/混淆的代码
- 数据而非指令

**修复**：
1. 换一个函数 hook
2. 或者用 `ii_install_at_return` 从返回点进入
3. 或者用 `ii_install_before_call` 从调用点进入

---

### -6 `ZH_E_NOMEM` 内存不足

**触发**：`mmap` / `malloc` 失败。

**排查**：
```bash
adb shell "cat /proc/<pid>/status | grep VmSize"
```

**修复**：卸载不用的 hook 释放内存。

**注意**：此码在现行实现中较少见（多在 mmap 池时）。

---

### -7 `ZH_E_WIDE` 无法覆盖 16 字节

**触发**：4 字节 `b` 跳不到（距离 > ±128 MB），要退 16 字节模式，但函数开头 4 条指令中有：
- 分支指令（`b`/`bl`/`b.cond`/`cbz`/`tbz`）
- `pc` 相对指令（`adr`/`adrp`/`ldr literal`）

**修复**：换函数。

---

### -9 `ZH_E_ALLOC` 分配失败

**触发**：内部 `malloc` 失败（站点结构、回调节点）。

**修复**：减少站点数。

---

### -10 `ZH_E_PATCH` 写指令失败

**触发**：
- `mprotect` 失败
- 地址不可写
- 页面被系统保护

**排查**：

```c
// 手工验证
int prot = 0;
// 从 /proc/self/maps 读出该地址的权限
```

**修复**：
- 确认地址正确（是不是 `data` 段而非 `text`）
- 确认 `mprotect` 返回 0

---

### -11 `ZH_E_REACH` 跳转超范围

**触发**：找到的 shell 距离目标 > ±128 MB。

**含义**：池里所有槽位都太远。

**修复**：
1. 引擎会自动尝试 `pool_from_mmap_near` —— 但实测几乎总失败
2. 退 16 字节模式（如果可能）
3. 换函数

---

### -13 `ZH_E_TAIL` 后续指令不干净

**触发**：16 字节模式里，被搬走的 3 条指令之后还有分支指向搬到 trampoline 的指令。

**为什么危险**：

```asm
func:
    mov  x0, #1     ← 搬到 tramp+0
    mov  x1, #2     ← 搬到 tramp+4
    mov  x2, #3     ← 搬到 tramp+8
    ...
    b    func+0     ← ★ 跳到 func+0，但那里已被改写
```

**修复**：换函数。

---

### -15 `ZH_E_SHELL` shell 分配失败

**触发**：池里没有可用槽位。

**原因**：
1. 池全满（4700 个都用完了）
2. 池全失效（`POOL DEAD`）
3. 槽位在 3 秒隔离区

**排查**：

```c
ii_engine_stats st;
ii_engine_get_stats(ii_default(), &st);
LOG_E(LOG_POOL, "站点 %d, 池死亡 %ld",
      st.nSites, st.poolDeaths);
```

**修复**：
- 等 3 秒（隔离期）
- 卸载不用的 hook
- 加大池（重新采集 `poolhint`）

---

### -16 `ZH_E_NOMULTI` 不支持多回调

**触发**：向 LIGHT 站点加多回调。

**原因**：LIGHT 模式只有单个回调槽。

**修复**：改用 FULL 模式（`zh_hook`）。

---

### -17 `ZH_E_SUSPEND` 暂停线程失败

**触发**：16 字节模式需要暂停线程，但有线程处于不可中断状态（`D` 状态）。

**排查**：

```bash
adb shell "ls /proc/<pid>/task/ | while read t; do cat /proc/<pid>/task/$t/stat | awk '{print $3}' | grep D && echo $t; done"
```

**修复**：
- 等一等重试
- 该函数是否必须 16 字节模式？能否换函数？

---

## 三、辅助函数

### 3.1 `ii_strerror()`

```c
const char* ii_strerror(int rc);
```

**返回**：简短错误名。

```c
LOG_E(LOG_HOOK, "rc=%d (%s)", rc, ii_strerror(rc));
// rc=-15 (shell allocation failed)
```

### 3.2 `ii_strhint()`

```c
const char* ii_strhint(int rc);
```

**返回**：修复建议（整句）。

```c
LOG_I(LOG_HOOK, "建议: %s", ii_strhint(rc));
// 建议: trampoline 池已满或失效。等待 3 秒后重试，或 zh_unhook 释放槽位。
```

### 3.3 示例

```c
static int try_hook(const char* fn, ii_callback cb, void* user) {
    int rc = zh_hook(fn, cb, user);
    if (rc == 0) {
        LOG_I(LOG_HOOK, "✓ %s", fn);
        return 0;
    }

    LOG_E(LOG_HOOK, "✗ %s rc=%d (%s)", fn, rc, ii_strerror(rc));
    LOG_I(LOG_HOOK, "   %s", ii_strhint(rc));

    // 自动重试一次（针对 -15）
    if (rc == -15) {
        sleep(4);   // 等隔离期过
        rc = zh_hook(fn, cb, user);
        if (rc == 0) {
            LOG_I(LOG_HOOK, "✓ %s（重试成功）", fn);
            return 0;
        }
    }
    return rc;
}
```

---

## 四、安装失败排查流程

```
zh_hook 失败
  │
  ├─ rc == -1 → 函数名不在表？
  │              └─ zh_find(fn) == 0 ? → 用 zh_tbl_retfunc 枚举
  │
  ├─ rc == -2 → 已有站点
  │              └─ 用 ii_add_callback
  │
  ├─ rc == -4 → 没初始化
  │              └─ 先 zh_layer_init_default
  │
  ├─ rc == -5 → 指令不可插桩
  │              ├─ 换函数
  │              └─ 用 ii_install_at_return
  │
  ├─ rc == -7 → 16 字节不行
  │              └─ 换函数
  │
  ├─ rc == -11 → 池太远
  │              └─ 换函数 或 加池
  │
  ├─ rc == -13 → 后续指令不干净
  │              └─ 换函数
  │
  ├─ rc == -15 → 无槽位
  │              ├─ 等 3 秒
  │              ├─ zh_unhook 释放
  │              └─ 查池状态
  │
  └─ rc == -17 → 暂停失败
                 └─ 重试 或 换函数
```

---

## 五、运行时错误

**这些不是返回值，而是日志里出现的内容：**

| 日志 | 含义 | 处理 |
|---|---|---|
| `[SLOW] 回调 ... 耗时 N ms` | 回调太慢 | 移到异步 |
| `[POOL DEAD] #N` | 池失效 | 引擎自动恢复 |
| `[RECOVER] #N -> 0` | 恢复成功 | 无需处理 |
| `[RECOVER] #N -> -1` | 恢复失败 | 卸载相关 hook |
| `FATAL: 站点 #N 无回调` | 站点结构损坏 | 上报 bug |

---

## 六、错误码与 Mixin 对比

| Mixin 异常 | Nexus 错误码 |
|---|---|
| `MixinApplyError` | -5 / -13 |
| `InvalidInjectionException` | -1 / -5 |
| `InvalidMixinException` | -1 / -2 |
| `ClassNotFoundException` | -1（数据表没有） |
| `NoSuchMethodError` | -1 |

---

## 下一步

- [性能与开销](41-Reference-Performance)
- [常见陷阱](52-Guide-Pitfalls)
- [FAQ](90-FAQ)
