# 参考：性能与开销

实测数据，来自 t01–t13 真机测试。

---

## 一、hook 安装/卸载开销

| 操作 | 模式 | 耗时 |
|---|---|---|
| `zh_hook` 首次（4B 模式） | FULL | **~120 μs** |
| `ii_add_callback` | — | ~2 μs |
| `zh_unhook`（4B 模式） | FULL | ~80 μs |
| `zh_hook`（16B 模式） | FULL | ~2.5 ms（含线程暂停） |
| `zh_hook_light` | LIGHT | ~100 μs |

**4 字节模式很快**（无锁），**16 字节模式慢 20 倍**（要暂停所有线程）。

---

## 二、回调执行开销

### 2.1 单次回调（不含你的代码）

| 场景 | 耗时 |
|---|---|
| LIGHT 模式 | **~120 ns** |
| FULL 模式 | **~280 ns** |
| FULL + 隔离 | **~305 ns** |
| FULL + 多回调链（3 个） | ~840 ns |
| 返回点 hook | ~280 ns |
| 重定向 | ~340 ns |

**分解（FULL 模式 280 ns）**：

| 步骤 | 耗时 |
|---|---|
| shell（4 条指令） | ~5 ns |
| `ii_common` 保存 31 个 x + 32 个 q | ~180 ns |
| 调 `ii_dispatch` | ~40 ns |
| 你的回调（空） | ~10 ns |
| 恢复寄存器 | ~180 ns |
| 总 | **~280 ns** |

**为什么保存/恢复占大头**：`q[32]` 是 **512 字节**的 SIMD 寄存器，`stp q, q, [sp], #32` × 16 次。

### 2.2 用 LIGHT 省多少

**LIGHT 只保存 `x0-x7` + `q0-q1`**：

```
8 × 8 + 2 × 16 = 96 字节
```

**vs FULL 的 800 字节 → 快 2.3 倍。**

---

## 三、实测吞吐（t04 / t08d）

### 3.1 t04：20 TPS tick

```
函数：LocalPlayer_normalTick（20 次/秒）
安装：zh_hook_light
运行：60 秒
结果：1200 次回调，零丢帧，游戏稳定 20 TPS
```

### 3.2 t08d：30 秒压力

```
函数：LocalPlayer_normalTick
回调链：3 个（priority 300/100/0）
运行：30 秒
结果：
  - 601 次回调（20 TPS × 30 秒 = 600，+1 边界）
  - 零崩溃
  - 零丢帧
  - 游戏 FPS 无变化
```

**601 次 / 30 秒 = 20.03 TPS。**

### 3.3 t06：字段读取

```
函数：高频 tick
操作：zh_field_get × 3000 次/秒
运行：持续
结果：零开销可见
```

### 3.4 t07：栈局部变量（最热的场景）

```
函数：ItemStack_isNull
频率：74844 次 / 3 秒 ≈ 25000 次/秒
操作：zh_local_get × 4
结果：稳定运行，游戏无卡顿
```

**25,000 次/秒 × 280 ns = 7 ms/秒 = 0.7% CPU 单核。**

---

## 四、隔离开销

| 场景 | 耗时 | 相对开销 |
|---|---|---|
| 隔离关 | 280 ns | 基准 |
| 隔离开（正常） | 305 ns | **+9%** |
| 隔离捕获一次崩溃 | ~2 μs | 一次性 |

**`setjmp` 在 ARM64 保存 ~30 个寄存器 → ~25 ns。**

---

## 五、内存占用

### 5.1 库本身

| 项目 | 大小 |
|---|---|
| `libzh_engine.so` | 841,984 B（约 822 KB） |
| 其中数据表 | 367,193 B |
| 代码 + 常量 | 约 455 KB |

### 5.2 运行时

| 项目 | 大小 |
|---|---|
| 站点结构（每站点） | 约 120 B |
| 回调节点（每个） | 约 48 B |
| shell 槽位（每个） | 128 B |
| 共享区文件 | 512 KB（`mmap`） |
| 日志缓冲 | 64 KB |

**100 个站点 + 200 个回调**：

```
100 × 120 + 200 × 48 + 100 × 128 = 12000 + 9600 + 12800 = 34.4 KB
```

**极小。**

---

## 六、启动开销

| 阶段 | 耗时 |
|---|---|
| `zlog_init` | < 1 ms |
| `ii_init` | ~5 ms |
| 查找模块基址 | ~2 ms |
| 版本指纹 | ~15 ms |
| 注册 hint 池 | ~3 ms |
| 数据表自检 | ~8 ms |
| **总** | **~35 ms** |

**相对于游戏启动（数秒）可忽略。**

---

## 七、多实例开销

| key 数量 | 每次 dispatch 开销 |
|---|---|
| 1 | +20 ns |
| 8 | +40 ns |
| 16 | +70 ns |
| 64 | +250 ns |
| 256 | +1 μs |

**线性扫描。超过 256 个 key 建议自己建 hash。**

---

## 八、日志开销

| 级别 | 每行耗时 |
|---|---|
| 格式 + 进缓冲 | ~200 ns |
| 缓冲满写盘 | ~50 μs（一次性） |

**每秒 200 行日志 ≈ 40 μs/秒 CPU**（可忽略）。

**但日志文件增长**：约 **100 B/行 → 20 KB/秒 → 1.2 MB/分钟**。

---

## 九、池容量限制

| 池 | 大小 | 槽位（128 B） |
|---|---|---|
| hint #0 | 308 KB | 2,464 |
| hint #1 | 244 KB | 1,952 |
| hint #2 | 20 KB | 160 |
| hint #3 | 8 KB | 64 |
| hint #4 | 7 KB | 56 |
| **合计** | **587 KB** | **约 4,700** |

**4,700 个站点足够绝大多数 Mod。**

---

## 十、绑核 / 调度

**Nexus 不改变线程调度。**

**但注意**：

1. 回调运行在**调用者的线程**上
2. 高频函数可能在渲染线程 → 你的回调影响 FPS
3. 建议：只在回调里做 O(1) 的事，重活丢给工作线程

---

## 十一、对比表

| 场景 | Nexus FULL | Nexus LIGHT | 传统 JNI |
|---|---|---|---|
| 单次开销 | 280 ns | 120 ns | N/A |
| 20 TPS × 1 hook | 5.6 μs/秒 | 2.4 μs/秒 | — |
| 25k 次/秒 × 1 hook | 7 ms/秒（0.7%） | 3 ms/秒（0.3%） | — |
| 安装耗时 | 120 μs | 100 μs | — |
| 卸载耗时 | 80 μs | — | — |
| 内存/站点 | 120 B + 128 B | 120 B + 128 B | — |

---

## 十二、优化建议

| 场景 | 建议 |
|---|---|
| 20+ TPS 的 tick 函数 | 用 `zh_hook_light` |
| 万次/秒的热函数 | LIGHT + 关隔离 |
| 不常用函数 | FULL + 隔离 |
| 需要改返回值 | 必须 FULL（返回点） |
| key 数 > 256 | 自己建 hash |
| 只要统计 | 用 `__atomic` 计数器，不打日志 |

**LIGHT 的代价**：不能 cancel，不能改返回值，只能看前 8 个参数。

---

## 十三、如何测量你自己的开销

```c
static uint64_t g_totalNs = 0;
static unsigned long g_calls = 0;

static void on_tick(ii_ctx* c, void* user) {
    (void)user;

    uint64_t t0 = zh_now_ns();

    // ═══ 你的逻辑 ═══
    void* p = (void*)c->x[0];
    if (p) {
        int64_t v = zh_field_get(c, 0, 0x250, 8);
        (void)v;
    }
    // ══════════════

    uint64_t dt = zh_now_ns() - t0;
    __atomic_add_fetch(&g_totalNs, dt, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_calls, 1, __ATOMIC_RELAXED);
}

static void* stats(void* arg) {
    (void)arg;
    unsigned long lastCalls = 0;
    uint64_t lastNs = 0;

    while (1) {
        sleep(5);
        unsigned long calls = __atomic_load_n(&g_calls, __ATOMIC_RELAXED);
        uint64_t ns = __atomic_load_n(&g_totalNs, __ATOMIC_RELAXED);

        unsigned long dc = calls - lastCalls;
        uint64_t dn = ns - lastNs;
        if (dc > 0) {
            LOG_I(LOG_HOOK, "回调 %lu 次/5s，平均 %llu ns/次，总占 %.2f%%",
                  dc, dn / dc, (double)dn / 5000000000.0 * 100.0);
        }
        lastCalls = calls;
        lastNs = ns;
    }
    return NULL;
}
```

---

## 下一步

- [错误码](40-Reference-Error-Codes)
- [数据格式](42-Reference-Data-Format)
- [常见陷阱](52-Guide-Pitfalls)
