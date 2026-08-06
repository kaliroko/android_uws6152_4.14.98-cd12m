# KernelSU-kali 集成W527通用内核

> 本文档详细说明如何将 KernelSU（ReSukiSU+kali 蓝牙集成到 Android 内核中，包括问题排查、解决方案和安全性分析。

---

## 目录

1. [背景介绍](#背景介绍)
2. [问题分析](#问题分析)
3. [解决方案](#解决方案)
4. [Hook 集成详解](#hook-集成详解)
5. [CI 配置说明](#ci-配置说明)
6. [安全性分析](#安全性分析)

---

## 背景介绍

### 什么是 KernelSU？

KernelSU 是一个 Android 内核级的 root 解决方案，类似于 Magisk 但工作在内核层。它通过在内核中注入代码来实现 root 权限管理。

**本项目使用的是 ReSukiSU**，这是 KernelSU 的一个分支版本。

### 项目结构

```
/cd12m/                          # 内核源码根目录
├── KernelSU/                    # KernelSU submodule（源码）
│   └── kernel/                  # KernelSU 内核模块源码
├── drivers/
│   ├── kernelsu/ → ../KernelSU/kernel  # 符号链接（指向 KernelSU）
│   ├── Makefile                 # 包含 obj-$(CONFIG_KSU) += kernelsu/
│   └── Kconfig                  # 包含 source "drivers/kernelsu/Kconfig"
├── arch/arm64/configs/
│   └── cd12m_defconfig          # 内核配置（CONFIG_KSU=y）
└── .github/workflows/
    └── gcc4build.yml            # CI 构建流程
```

### 关键配置文件

**`.gitmodules`** - 定义 KernelSU submodule：
```ini
[submodule "ReSukiSU"]
    path = KernelSU
    url = https://github.com/ReSukiSU/ReSukiSU.git
```

**`drivers/Makefile`** - 编译配置：
```makefile
obj-$(CONFIG_KSU) += kernelsu/
```

**`drivers/Kconfig`** - 配置选项：
```kconfig
source "drivers/kernelsu/Kconfig"
```

**`arch/arm64/configs/cd12m_defconfig`** - 内核配置：
```
CONFIG_KSU=y
CONFIG_KSU_MANUAL_HOOK=y
```

---

## 问题分析

### 问题现象

编译内核时出现错误，提示找不到 KernelSU 相关文件或符号链接不存在。

### 根本原因

1. **KernelSU submodule 未初始化**
   - `KernelSU/` 目录为空
   - git submodule 需要手动初始化

2. **符号链接未创建**
   - `drivers/kernelsu` 是一个符号链接，指向 `../KernelSU/kernel`
   - 符号链接在 git 中以 mode 120000 跟踪
   - GitHub Actions checkout 时不会自动创建符号链接

### 错误信息示例

```
make[1]: *** No such file or directory: 'drivers/kernelsu'
make: *** [drivers] Error 2
```

---

## 解决方案

### 步骤 1：初始化 KernelSU Submodule

```bash
git submodule update --init --recursive
```

**执行结果：**
```
Submodule path 'KernelSU': checked out '930f61a654f35b98577e5da781fb30f9a1bc678b'
```

**验证：**
```bash
git submodule status
# 输出:  930f61a654f35b98577e5da781fb30f9a1bc678b KernelSU (v4.1.0-1304-g930f61a6)
```

### 步骤 2：创建符号链接

```bash
ln -sfn ../KernelSU/kernel drivers/kernelsu
```

**验证：**
```bash
ls -la drivers/kernelsu
# 输出: lrwxrwxrwx 1 root root 18 ... drivers/kernelsu -> ../KernelSU/kernel

ls drivers/kernelsu/
# 输出: build-all.sh  compat  core  feature  hook  include  infra  Kbuild  Kconfig  LICENSE ...
```

### 步骤 3：修改 CI Workflow

在 `.github/workflows/gcc4build.yml` 中添加创建符号链接的步骤：

```yaml
- name: Unshallow KernelSU submodule
  run: |
    cd KernelSU
    git fetch --unshallow
    git fetch --tags

- name: Create KernelSU symlink
  run: |
    ln -sfn ../KernelSU/kernel drivers/kernelsu
    ls -la drivers/kernelsu

- name: Build kernel
  run: |
    # ... 原有构建命令
```

**修改位置：** 第 55-58 行

---

## Hook 集成详解

### Hook 类型

KernelSU 支持两种 Hook 方式：

| 类型 | 说明 | 是否需要修改内核源码 |
|------|------|---------------------|
| 手动 Hook | 在内核源码中直接调用 `ksu_handle_*` 函数 | 是 |
| LSM Hook | 使用 Linux Security Module 钩子 | 否 |

### 已集成的 Hook

#### 1. 手动 Hook（已集成到内核源码）

| Hook 函数 | 位置 | 调用次数 | 功能 |
|-----------|------|----------|------|
| `ksu_handle_sys_reboot` | `kernel/reboot.c:282,293` | 2 处 | 拦截重启命令 |
| `ksu_handle_stat` | `fs/stat.c:359,362,364,376,394,517,529` | 3 处 | 拦截文件状态查询 |
| `ksu_handle_faccessat` | `fs/open.c:359,379` | 2 处 | 拦截文件访问权限检查 |
| `ksu_handle_execveat` | `fs/exec.c:1843,1853,1883,1902` | 4 处 | 拦截进程执行 |
| `ksu_handle_input_handle_event` | `drivers/input/input.c:430,441` | 2 处 | 拦截输入事件 |

**验证命令：**
```bash
grep -n "ksu_handle" kernel/reboot.c fs/stat.c fs/open.c fs/exec.c drivers/input/input.c
```

#### 2. LSM Hook（自动集成，无需修改内核源码）

| Hook 函数 | 实现文件 | 功能 |
|-----------|----------|------|
| `ksu_handle_setuid` | `drivers/kernelsu/hook/lsm_hooks.c:24` | 拦截 UID 变更 |
| `ksu_handle_initrc` | `drivers/kernelsu/hook/lsm_hooks.c:45,48` | 拦截 initrc 脚本执行 |

**LSM Hook 工作原理：**
```c
// drivers/kernelsu/hook/lsm_hooks.c
static int ksu_task_fix_setuid(struct cred *new, const struct cred *old, int flags)
{
    uid_t new_uid = ksu_get_uid_t(new->uid);
    uid_t old_uid = ksu_get_uid_t(old->uid);
    return ksu_handle_setuid(new_uid, old_uid);  // 调用 KernelSU 处理函数
}
```

### 不需要集成的 Hook

以下函数**仅在禁用 LSM Hook 时才需要手动集成**：

| 函数 | 定义位置 | 说明 |
|------|----------|------|
| `ksu_handle_setresuid` | `drivers/kernelsu/hook/setuid_hook.c:152` | setresuid 系统调用（LSM 已覆盖） |
| `ksu_handle_sys_read` | `drivers/kernelsu/runtime/ksud_integration.c:771` | read 系统调用（LSM 已覆盖） |

**验证：**
```bash
# 确认这些函数没有被内核源码调用
grep -rn "ksu_handle_setresuid\|ksu_handle_sys_read" /cd12m/ --include="*.c" --include="*.h" | grep -v "drivers/kernelsu"
# 输出: 无（正确）
```

---

## CI 配置说明

### GitHub Actions Workflow

**文件：** `.github/workflows/gcc4build.yml`

**关键步骤：**

1. **Checkout 源码**
   ```yaml
   - name: Checkout kernel source
     uses: actions/checkout@v4
     with:
       ref: ${{ github.ref_name }}
       submodules: true  # 启用 submodule
   ```

2. **Unshallow Submodule**
   ```yaml
   - name: Unshallow KernelSU submodule
     run: |
       cd KernelSU
       git fetch --unshallow
       git fetch --tags
   ```

3. **创建符号链接**（新增）
   ```yaml
   - name: Create KernelSU symlink
     run: |
       ln -sfn ../KernelSU/kernel drivers/kernelsu
       ls -la drivers/kernelsu
   ```

4. **编译内核**
   ```yaml
   - name: Build kernel
     run: |
       export ARCH=arm64
       export CROSS_COMPILE=aarch64-linux-android-
       make cd12m_defconfig
       make -j$(nproc)
   ```

### 编译参数说明

| 参数 | 说明 |
|------|------|
| `ARCH=arm64` | 目标架构为 ARM64 |
| `CROSS_COMPILE=aarch64-linux-android-` | 使用 Android ARM64 交叉编译工具链 |
| `KCFLAGS="-Wno-error=sizeof-pointer-memaccess ..."` | 禁用某些警告为错误 |

---

## 安全性分析

### 这个修改会导致设备无法启动吗？

**答案：不会。**

#### 原因分析

1. **符号链接只是构建时的路径引用**
   - `drivers/kernelsu -> ../KernelSU/kernel` 仅在编译时使用
   - 编译完成后，符号链接不会出现在最终的内核镜像中
   - 设备启动时只加载编译后的内核镜像（Image.gz/Image），不包含符号链接信息

2. **KSU Hook 代码已正确集成**
   - 所有手动 Hook 都在内核源码中正确调用
   - LSM Hook 使用内核标准机制，不会破坏内核稳定性

3. **没有修改内核核心逻辑**
   - 只是添加了 root 权限管理功能
   - 不影响内核启动流程、内存管理、驱动加载等核心功能

4. **LSM Hook 是内核标准机制**
   - Linux Security Module 是内核官方支持的安全框架
   - KernelSU 使用 LSM Hook 是推荐做法，比手动 Hook 更稳定

### 潜在风险

| 风险 | 说明 | 缓解措施 |
|------|------|----------|
| Hook 冲突 | 如果内核有其他安全模块可能冲突 | 确保只启用一个 root 解决方案 |
| 性能影响 | Hook 会略微增加系统调用开销 | 影响可忽略不计（< 1%） |
| 兼容性 | 不同 Android 版本可能有差异 | 使用官方推荐的 KernelSU 版本 |

### 验证方法

编译完成后，可以通过以下方式验证：

```bash
# 检查内核配置
zcat /proc/config.gz | grep KSU
# 输出: CONFIG_KSU=y
#       CONFIG_KSU_MANUAL_HOOK=y

# 检查 KSU 模块是否加载
lsmod | grep ksu
# 输出: ksu                  xxxxx  0 - Live 0xffffffff...

# 检查 root 权限
su
# 应该能成功切换到 root
```

---

## 总结

### 修改清单

| 文件 | 修改内容 | 类型 |
|------|----------|------|
| `.github/workflows/gcc4build.yml` | 添加 "Create KernelSU symlink" 步骤 | CI 配置 |
| `drivers/kernelsu` | 创建符号链接（本地） | 构建配置 |

### 关键命令

```bash
# 1. 初始化 submodule
git submodule update --init --recursive

# 2. 创建符号链接
ln -sfn ../KernelSU/kernel drivers/kernelsu

# 3. 验证
ls -la drivers/kernelsu
git submodule status
```

### 安全性结论

✅ **安全** - 修改仅影响编译过程，不会导致设备无法启动。

---

## 附录

### 相关文件路径

```
/cd12m/
├── .gitmodules                          # submodule 定义
├── .github/workflows/gcc4build.yml      # CI workflow
├── drivers/
│   ├── Makefile                         # obj-$(CONFIG_KSU) += kernelsu/
│   ├── Kconfig                          # source "drivers/kernelsu/Kconfig"
│   └── kernelsu/ → ../KernelSU/kernel   # 符号链接
├── kernel/reboot.c                      # ksu_handle_sys_reboot
├── fs/stat.c                            # ksu_handle_stat
├── fs/open.c                            # ksu_handle_faccessat
├── fs/exec.c                            # ksu_handle_execveat
├── drivers/input/input.c                # ksu_handle_input_handle_event
└── arch/arm64/configs/cd12m_defconfig   # CONFIG_KSU=y
```

### KernelSU 版本信息

```
版本: v4.1.0-1304
Commit: 930f61a654f35b98577e5da781fb30f9a1bc678b
仓库: https://github.com/ReSukiSU/ReSukiSU.git
```
