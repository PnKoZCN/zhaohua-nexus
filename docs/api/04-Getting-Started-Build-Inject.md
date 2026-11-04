# 构建与注入

本页详解如何构建引擎与 Mod，并正确注入到游戏进程。

---

## 一、构建引擎

### 1.1 环境准备

```bash
# Android NDK r27c（必须，musl gcc 编译的 .so 无法在 Android 加载）
export NDK=/opt/android-ndk-r27c

# 验证
$NDK/ndk-build --version   # 应输出 r27c
```

### 1.2 标准构建

```bash
cd zh_engine

cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build -j8
```

**产物**：`build/libzh_engine.so`（约 840 KB）

### 1.3 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `ZH_BUILD_TESTS` | OFF | 主机侧单元测试 |
| `ZH_BUILD_EXAMPLES` | OFF | 真机示例模块 |
| `ZH_BUILD_STATIC` | OFF | 同时构建静态库 |
| `ZH_USE_ASAN` | OFF | AddressSanitizer（调试用） |
| `ZH_WERROR` | OFF | 警告视为错误 |
| `ZH_STRICT` | OFF | 严格警告（-Wall -Wextra） |
| `ZH_PORTABLE` | OFF | 去 bionic 依赖（可在普通 Linux 加载） |

### 1.4 主机侧构建（用于 CI）

```bash
cmake -B build-host -DCMAKE_BUILD_TYPE=Release -DZH_BUILD_TESTS=ON
cmake --build build-host -j8

cd build-host
for t in tests/test_*; do
    [ -x "$t" ] || continue
    LD_LIBRARY_PATH=. ./$t >/dev/null 2>&1 \
        && echo "OK   $(basename $t)" \
        || echo "FAIL $(basename $t)"
done
```

**期望：12/12 通过。**

### 1.5 校验产物

```bash
F=build/libzh_engine.so

# 1. 页对齐必须是 0x4000（Android 15+ 要求）
readelf -lW $F | grep LOAD | head -1 | awk '{print $NF}'

# 2. 不能有 TLS 段 / TLSDESC 重定位（musl 会引入，Android 不支持）
readelf -lW $F | grep -c TLS          # 应为 0
readelf -rW $F | grep -c TLSDESC      # 应为 0

# 3. 不能有文本重定位（W^X）
readelf -dW $F | grep -c TEXTREL     # 应为 0

# 4. 栈不可执行
readelf -lW $F | grep GNU_STACK       # 应为 RW

# 5. 符号数
readelf --dyn-syms -W $F | grep -cE ' (zh|ii|zlog)_'   # 应为 189
```

---

## 二、构建你的 Mod

### 2.1 目录结构建议

```
my_project/
├── CMakeLists.txt
├── my_mod.c
└── zh_engine/          （引擎源码，作为子模块或软链）
```

### 2.2 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_mod C)

# 方式 A：引擎源码在子目录
add_subdirectory(zh_engine)
add_library(my_mod SHARED my_mod.c)
target_link_libraries(my_mod PRIVATE zh_engine)
target_include_directories(my_mod PRIVATE ${CMAKE_SOURCE_DIR}/zh_engine/include)

# 方式 B：用已构建好的引擎（更快）
# add_library(zh_engine SHARED IMPORTED)
# set_target_properties(zh_engine PROPERTIES
#     IMPORTED_LOCATION /path/to/libzh_engine.so
#     INTERFACE_INCLUDE_DIRECTORIES /path/to/zh_engine/include)
# add_library(my_mod SHARED my_mod.c)
# target_link_libraries(my_mod PRIVATE zh_engine)
```

### 2.3 构建

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

**产物**：`build/libmy_mod.so`

### 2.4 必要的校验

```bash
F=build/libmy_mod.so

# 必须依赖 libzh_engine.so
readelf -dW $F | grep NEEDED
# 期望包含 libzh_engine.so

# 页对齐
readelf -lW $F | grep LOAD | head -1 | awk '{print $NF}'   # 0x4000

# 无 TLS / TLSDESC / VERDEF
readelf -lW $F | grep -c TLS         # 0
readelf -rW $F | grep -c TLSDESC     # 0
readelf -dW $F | grep -c VERDEF      # 0
```

---

## 三、注入

### 3.1 注入原理

Nexus 不修改 APK，而是在游戏运行时把 `.so` 加载进进程。

**核心是调用游戏的 `dlopen`**：

```c
// zhinf 做的事（简化）
void* handle = ((void*(*)(const char*, int))dlopen_addr)(lib_path, RTLD_NOW);
```

**需要两个地址**：
1. `mmap` —— 用来分配内存放路径字符串
2. `dlopen` —— 真正的加载函数

**这两个地址每次游戏启动都会变（ASLR），需要重新查。**

### 3.2 查找地址

```bash
PID=$(ps -A | grep 'com.netease.x19$' | awk '{print $2}' | head -1)

# 找 libc 的 mmap
LIBC_BASE=$(grep 'libc.so' /proc/$PID/maps | head -1 | cut -d- -f1)
# mmap 在 libc 里的偏移需要查（不同版本不同）
# 常见值：0xF0000 左右

# 找 libdl 的 dlopen
LIBDL_BASE=$(grep 'libdl.so' /proc/$PID/maps | head -1 | cut -d- -f1)
# dlopen 偏移常见：0x101c

echo "libc  0x$LIBC_BASE"
echo "libdl 0x$LIBDL_BASE"
```

**更可靠的方法**：用 `/proc/$PID/maps` 找到的段起点加上已知偏移，或用 nm：
```bash
# 从设备拉 libc.so 和 libdl.so 后查
nm -D --defined-only libc.so | grep " T mmap$"
nm -D --defined-only libdl.so | grep " T dlopen$"
```

### 3.3 完整注入流程

```bash
#!/bin/bash
# inject.sh

PID=$(ps -A | grep 'com.netease.x19$' | awk '{print $2}' | head -1)
[ -z "$PID" ] && { echo "游戏未运行"; exit 1; }

LIBDIR=$(ls -d /data/app/*/com.netease.x19*/lib/arm64 | head -1)

# 1. 部署
cp libzh_engine.so $LIBDIR/
chmod 755 $LIBDIR/libzh_engine.so

# 2. 算地址（实际需按版本查）
LIBC=$(grep 'libc.so' /proc/$PID/maps | head -1 | cut -d- -f1)
LIBDL=$(grep 'libdl.so' /proc/$PID/maps | head -1 | cut -d- -f1)
MMAP=$((0x$LIBC + 0xF0000))
DLOPEN=$((0x$LIBDL + 0x101c))

# 3. 注入引擎
./zhinf $PID libzh_engine.so $(printf 0x%lx $MMAP) $(printf 0x%lx $DLOPEN)

# 4. 注入 Mod
cp libmy_mod.so $LIBDIR/
chmod 755 $LIBDIR/libmy_mod.so
./zhinf $PID libmy_mod.so $(printf 0x%lx $MMAP) $(printf 0x%lx $DLOPEN)

# 5. 立即删除（磁盘应保持干净）
rm $LIBDIR/libmy_mod.so
```

### 3.4 注入前检查清单

**每次注入前必须确认：**

```bash
# 1. 进程名正确
cat /proc/$PID/cmdline | tr '\0' ' '; echo   # com.netease.x19

# 2. 线程数 > 50（确认是主进程不是子进程）
grep Threads /proc/$PID/status

# 3. 状态是 S 或 R（不是 D/Z）
grep '^State' /proc/$PID/status

# 4. 目标库还没被注入
grep -c libzh_engine /proc/$PID/maps   # 应为 0

# 5. 游戏 so 段数正常
grep -c libminecraftpe /proc/$PID/maps  # 应为 7
```

**自动化脚本**（`_precheck4.py`）：

```bash
python3 _precheck4.py $PID [允许已加载的库名]
# 输出 11 项检查结果，全部 OK 才能注入
```

---

## 四、验证

### 4.1 确认库已加载

```bash
grep libzh_engine /proc/$PID/maps | wc -l
# 应该 >= 3（多个段）
```

### 4.2 看引擎日志

```bash
cat /data/data/com.netease.x19/files/zhprobe/watchdog.log
```

**正常启动的样子**：
```
id=1 启动 watchdog（各管各的站点）
watchdog start id=1 tid=12345 pools=0
hint 池 + vaddr 0x1334b08c 308 KB
...
heartbeat id=1 ticks=20 pools=5 deaths=0 recov=0
```

### 4.3 确认 hook 生效

**你的 Mod 应该输出日志或计数。** 若没有：

```bash
# 检查引擎是否真的装上了
# 在 Mod 里加：
unsigned long a = zh_find("LocalPlayer_normalTick");
LOG_I(LOG_HOOK, "addr=0x%lx", a);
int rc = zh_hook("LocalPlayer_normalTick", cb, NULL);
LOG_I(LOG_HOOK, "hook rc=%d", rc);
```

### 4.4 卸载清理

```bash
# 删除注入的 Mod
rm $LIBDIR/libmy_mod.so

# 如果引擎也要卸（一般不用，可以常驻）
# 删除文件后，已加载的副本仍驻留内存，重启游戏才真正卸掉
```

---

## 五、常见错误

### 5.1 `dlopen 调用失败 rc=-1`

| rc | 含义 | 处理 |
|---|---|---|
| `-1` | dlopen 返回 NULL | 库路径错误 / 依赖缺失 |
| `-2` | 找不到 mmap 地址 | 地址算错了 |
| `-3` | 找不到 dlopen 地址 | 地址算错了 |

**排查**：
```bash
# 1. 路径是否存在
ls -l $LIBDIR/libmy_mod.so

# 2. 依赖是否满足
readelf -dW $LIBDIR/libmy_mod.so | grep NEEDED
# libzh_engine.so 必须也在同目录

# 3. 权限
ls -l $LIBDIR/libzh_engine.so   # 应是 755
```

### 5.2 库加载了但没有日志

**`constructor` 可能没执行，或执行了但没写文件。**

```bash
# 1. 确认库真的在进程里
grep libmy_mod /proc/$PID/maps

# 2. 如果不在，说明 dlopen 失败（但 zhinf 报成功？）
#    检查 SELinux
dmesg | tail -20 | grep avc
```

### 5.3 注入后游戏崩溃

**按这个顺序查**：

1. **`constructor` 里阻塞？** → 看 [常见陷阱](52-Guide-Pitfalls)
2. **回调里有耗时操作？** → 看日志有没有 `[SLOW]`
3. **数据表版本不匹配？** → `zh_find` 返回 0 会早退，不会崩
4. **ANR？** → `adb shell ls /data/system/dropbox/ | grep anr`

---

## 六、部署检查清单

```
□ libzh_engine.so 已构建并通过二进制校验
□ libmy_mod.so 已构建并通过二进制校验
□ 两者都在游戏 lib 目录且权限 755
□ 进程 PID 正确（主进程，线程数 > 50）
□ mmap / dlopen 地址已正确计算
□ 注入前跑了 _precheck4.py 且全 OK
□ 注入后 30 秒内无崩溃
□ 日志无 [SLOW] 告警
□ 功能符合预期
□ 验证后删除了注入的 Mod .so
```

---

## 七、自动化建议

把校验做成一键脚本：

```bash
#!/bin/bash
# verify.sh —— 注入前完整校验

set -e
F=$1

echo "[1] 二进制校验"
readelf -lW $F | grep LOAD | head -1 | awk '{print "  页对齐: "$NF}'
[ "$(readelf -lW $F | grep -c TLS)" = "0" ]   || { echo "  ❌ 有 TLS 段"; exit 1; }
[ "$(readelf -rW $F | grep -c TLSDESC)" = "0" ] || { echo "  ❌ 有 TLSDESC"; exit 1; }
echo "  ✅"

echo "[2] 进程校验"
PID=$(ps -A | grep 'com.netease.x19$' | awk '{print $2}' | head -1)
python3 _precheck4.py $PID libzh_engine.so
```

---

## 下一步

- [站点与回调链原理](10-Core-Site-And-Chain)
- [错误码](40-Reference-Error-Codes)
- [常见陷阱](52-Guide-Pitfalls)
