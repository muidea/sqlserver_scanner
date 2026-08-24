# 构建指南

本文档介绍如何在不同平台上构建 sqlserver_scanner DuckDB 扩展。

## 前置条件

### 所有平台
- CMake 3.5 或更高版本
- C++17 编译器
- Git
- Make 构建工具

### Linux

在 Linux 上，需要安装系统依赖和 vcpkg：

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install -y build-essential cmake git freetds-dev unixodbc-dev

# CentOS/RHEL/Fedora
sudo yum groupinstall "Development Tools"
sudo yum install -y cmake git freetds-devel unixODBC-devel

# 或者使用 dnf (较新的 Fedora/RHEL)
sudo dnf groupinstall "Development Tools"
sudo dnf install -y cmake git freetds-devel unixODBC-devel
```

### Windows

在 Windows 上，需要：
- Visual Studio 2019 或更高版本（推荐）或 MinGW-w64
- vcpkg 包管理器（见下方安装说明）

## vcpkg 安装

vcpkg 用于管理项目的 C++ 依赖。支持三种安装方式：

### 方式 1：使用环境变量（推荐用于 CI/CD）

```bash
# 安装 vcpkg 到任意位置
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
cd ~/vcpkg

# Bootstrap vcpkg
# Linux/macOS:
./bootstrap-vcpkg.sh
# Windows (CMD):
.\bootstrap-vcpkg.bat
# Windows (PowerShell):
.\bootstrap-vcpkg.bat

# 设置环境变量
# Linux/macOS (添加到 ~/.bashrc 或 ~/.zshrc):
export VCPKG_ROOT=~/vcpkg

# Windows CMD:
set VCPKG_ROOT=C:\vcpkg

# Windows PowerShell (添加到 $PROFILE):
$env:VCPKG_ROOT="C:\vcpkg"
```

### 方式 2：相对路径安装（推荐用于本地开发）

在项目的父目录安装 vcpkg，Makefile 会自动检测：

```bash
# 在项目父目录安装
cd /path/to/workspace  # 假设项目在 /path/to/workspace/sqlserver_scanner
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg

# Bootstrap
./bootstrap-vcpkg.sh  # Linux/macOS
# 或
.\bootstrap-vcpkg.bat  # Windows
```

Makefile 会按以下优先级查找 vcpkg：
1. 环境变量 `VCPKG_ROOT`
2. 相对路径 `../vcpkg`
3. 相对路径 `./vcpkg`

### 方式 3：项目内嵌

将 vcpkg 安装到项目目录内：

```bash
# 在项目根目录
git clone https://github.com/microsoft/vcpkg.git vcpkg
cd vcpkg

# Bootstrap
./bootstrap-vcpkg.sh  # Linux/macOS
# 或
.\bootstrap-vcpkg.bat  # Windows
```

### 使用 Makefile 自动初始化

可以使用 Makefile 提供的目标自动安装 vcpkg：

```bash
make init-vcpkg
```

这会将 vcpkg 安装到 `../vcpkg` 目录。

## 构建

### 快速开始

```bash
# 1. 检查依赖
make check-deps

# 2. 构建 release 版本
make release

# 3. 运行测试
make test
```

### Linux 构建

```bash
# 确保系统依赖已安装
sudo apt install freetds-dev  # Ubuntu/Debian

# 构建
make release

# 或使用特定 vcpkg 路径
VCPKG_ROOT=/path/to/vcpkg make release

# 运行测试
make test

# Debug 构建
make debug
```

**注意：**
- 首次构建时，vcpkg 会自动下载并编译 `unixodbc`
- Linux 上的 freetds 使用系统包管理器安装，不通过 vcpkg
- 构建时间取决于您的机器性能

### Windows 构建

```bash
# 确保已安装 vcpkg（见上方说明）

# 使用默认编译器（MSVC）
make release

# 或指定 vcpkg 路径
set VCPKG_ROOT=C:\path\to\vcpkg  # CMD
$env:VCPKG_ROOT="C:\path\to\vcpkg"  # PowerShell
make release

# 运行测试
make test

# Debug 构建
make debug
```

**注意：**
- 首次构建时，vcpkg 会自动下载并编译 `freetds` 及其依赖（`openssl` 等）
- Windows 使用系统自带的 ODBC SDK，不需要额外安装
- 建议使用 `x64-windows-static` triplet 以避免运行时 DLL 依赖

## 构建选项

### 基本选项

```bash
# Release 构建（默认）
make release

# Debug 构建
make debug

# RelWithDebInfo 构建
make reldebug

# Release with assertions
make relassert
```

### 高级选项

```bash
# 使用 Ninja 生成器（更快）
GEN=ninja make release

# 跳过测试
SKIP_TESTS=1 make release

# 指定 vcpkg 路径
VCPKG_ROOT=/path/to/vcpkg make release

# 指定 vcpkg triplet
VCPKG_TARGET_TRIPLET=x64-linux make release

# 指定 DuckDB 源码路径
DUCKDB_SRCDIR=/path/to/duckdb make release

# 使用自定义链接器
CUSTOM_LINKER=lld make release
```

### 并行构建

```bash
# 使用所有 CPU 核心
make -j$(nproc) release  # Linux
make -j%NUMBER_OF_PROCESSORS% release  # Windows CMD
```

## 依赖说明

### Linux 依赖

| 依赖 | 来源 | 用途 |
|------|------|------|
| **unixodbc** | vcpkg | ODBC SDK 和驱动管理器 |
| **freetds** | 系统包管理器 | TDS 协议实现和 ODBC 驱动 |
| **build-essential** | 系统包管理器 | 编译工具链 |

安装命令：
```bash
sudo apt install freetds-dev build-essential cmake git
```

### Windows 依赖

| 依赖 | 来源 | 用途 |
|------|------|------|
| **ODBC SDK** | Windows SDK（系统自带） | ODBC 接口 |
| **SQL Server 驱动** | Windows（系统自带） | 推荐使用的 ODBC 驱动 |

Windows 不需要额外安装依赖，系统已包含所需的 ODBC SDK 和 SQL Server 驱动。

**注意：** 虽然可以通过 vcpkg 安装 FreeTDS，但推荐直接使用系统自带的 SQL Server 驱动。

## 辅助命令

### 查看帮助

```bash
make help
```

输出示例：
```
sqlserver_scanner - DuckDB ODBC Extension

Available targets:
  make release       - Build release version
  make debug         - Build debug version
  make test          - Run tests
  make clean         - Clean build artifacts
  make format        - Format source code
  make init-vcpkg    - Initialize vcpkg installation
  make check-deps    - Check dependencies

Environment variables:
  VCPKG_ROOT            - vcpkg installation path
  VCPKG_TARGET_TRIPLET  - Target triplet
```

### 检查依赖

```bash
make check-deps
```

输出示例：
```
Checking dependencies...

vcpkg: Found (/path/to/vcpkg)
FreeTDS: Found in system
ODBC: vcpkg unixodbc

Build configuration:
  Platform: Linux
  Triplet: x64-linux

Ready to build: Yes
```

### 清理构建

```bash
# 清理构建产物
make clean

# 完全清理（包括 vcpkg 缓存）
rm -rf build/
rm -rf vcpkg_installed/
```

### 代码格式化

```bash
# 格式化代码
make format

# 检查格式（不修改）
make format-check
```

## 故障排除

### 问题 1：vcpkg 未找到

**错误信息：**
```
vcpkg not found. Set VCPKG_ROOT or install vcpkg in ../vcpkg or ./vcpkg
```

**解决方案：**
```bash
# 选项 1：安装 vcpkg
make init-vcpkg

# 选项 2：设置环境变量
export VCPKG_ROOT=/path/to/vcpkg

# 选项 3：安装到相对路径
cd ..
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg && ./bootstrap-vcpkg.sh
```

### 问题 2：Linux 上 freetds 未找到

**错误信息：**
```
FreeTDS not found. Install with: sudo apt install freetds-dev
```

**解决方案：**
```bash
# Ubuntu/Debian
sudo apt install freetds-dev

# CentOS/RHEL
sudo yum install freetds-devel

# 验证安装
ls -l /usr/include/tds.h
```

### 问题 3：Windows 链接错误

**错误信息：**
```
无法解析的外部符号 _SQLConnect@...
```

**解决方案：**
1. 确保使用正确的 triplet：
   ```bash
   set VCPKG_TARGET_TRIPLET=x64-windows-static
   ```

2. 清理并重新构建：
   ```bash
   make clean
   make release
   ```

3. 检查 vcpkg 集成：
   ```bash
   cd vcpkg
   .\vcpkg integrate install
   ```

### 问题 4：CMake 配置错误

**错误信息：**
```
CMake Error: Could not find CMAKE_C_COMPILER
```

**解决方案：**
- Linux：安装编译工具链
  ```bash
  sudo apt install build-essential
  ```

- Windows：安装 Visual Studio 或 MinGW，并确保在 PATH 中

### 问题 5：vcpkg 构建超时

**错误信息：**
```
Building package <package>: timed out
```

**解决方案：**
1. 增加超时时间：
   ```bash
   export VCPKG_DEFAULT_BINARY_CACHE=~/.vcpkg-cache
   ```

2. 使用预编译的二进制缓存：
   ```bash
   # 在 vcpkg 目录
   ./vcpkg install unixodbc:freetds --binarysource=clear
   ```

### 问题 6：测试失败

**错误信息：**
```
Tests are skipped in this run...
```

**解决方案：**
这是正常行为（在 Docker 外运行时跳过测试）。要强制运行测试：

```bash
# 在 Linux Docker 容器中
LINUX_CI_IN_DOCKER=1 make test

# 或直接运行测试二进制文件
./build/release/test/unittest "test/odbc*"
```

## 依赖关系图

```
sqlserver_scanner
│
├── ODBC SDK
│   ├── Linux: unixodbc (vcpkg)
│   │   └── 提供 ODBC 接口和驱动管理器
│   │
│   └── Windows: Windows SDK (系统自带)
│       └── 提供 ODBC 接口
│
└── freetds
    ├── Linux: 系统包管理器 (apt/yum)
    │   ├── 提供 TDS 协议实现
    │   ├── 提供 ODBC 驱动
    │   └── 版本：取决于系统包源
    │
    └── Windows: vcpkg
        ├── 依赖: openssl (vcpkg)
        ├── 依赖: libmysql (vcpkg, Windows only)
        ├── 提供 TDS 协议实现
        ├── 提供 ODBC 驱动
        └── 版本：1.3.10
```

## 平台差异

| 特性 | Linux | Windows |
|------|-------|---------|
| **vcpkg 路径** | `VCPKG_ROOT` 或 `../vcpkg` | `VCPKG_ROOT` 或 `../vcpkg` |
| **ODBC SDK** | unixodbc (vcpkg) | Windows SDK (系统) |
| **推荐驱动** | FreeTDS (系统包) | SQL Server (系统自带) |
| **Triplet** | `x64-linux` | `x64-windows-static` |
| **编译器** | GCC/Clang | MSVC/MinGW |
| **首次构建时间** | ~5-10 分钟 | ~5-10 分钟 |
| **字符集设置** | 自动添加 FreeTDS 参数 | 使用系统默认 |

## ODBC 驱动配置

### Linux - FreeTDS

1. **安装 FreeTDS**：
   ```bash
   # Ubuntu/Debian
   sudo apt install freetds-dev freetds-bin tdsodbc
   
   # CentOS/RHEL
   sudo yum install freetds-devel freetds
   ```

2. **配置 FreeTDS**（`/etc/freetds/freetds.conf`）：
   ```ini
   [global]
       tds version = 8.0
       client charset = UTF-8
   
   [mssql2000]
       host = 192.168.1.100
       port = 1433
       tds version = 8.0
   
   [mssql2016]
       host = 192.168.1.101
       instance = SQL2016
       tds version = 8.0
   ```

3. **配置 ODBC**（`/etc/odbcinst.ini`）：
   ```ini
   [FreeTDS]
   Description=FreeTDS ODBC Driver
   Driver=/usr/lib/x86_64-linux-gnu/odbc/libtdsodbc.so
   Setup=/usr/lib/x86_64-linux-gnu/odbc/libtdsS.so
   ```

4. **连接字符串示例**：
   ```sql
   -- 使用 DSN 配置
   D ATTACH 'DSN=mssql2000;UID=sa;PWD=password;' AS db1 (TYPE sqlserver_scanner, READ_ONLY);
   
   -- 使用 Driver 连接
   D ATTACH 'Driver={FreeTDS};Server=192.168.1.100;Port=1433;Database=mydb;UID=sa;PWD=password;' AS db2 (TYPE sqlserver_scanner, READ_ONLY);
   ```

**注意：** 代码会自动在 Linux 上添加 `ClientCharset=UTF-8` 和 `TDS_Version=8.0` 参数。

### Windows - SQL Server 驱动

Windows 系统自带 SQL Server ODBC 驱动，无需额外安装。

1. **查看已安装的驱动**：
   ```powershell
   # PowerShell (管理员)
   reg query "HKLM\SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers"
   ```

2. **连接字符串示例**：
   ```sql
   -- SQL Server 驱动（推荐）
   D ATTACH 'Driver={SQL Server};Server=192.168.1.100,1433;Database=mydb;UID=sa;PWD=password;' AS db1 (TYPE sqlserver_scanner, READ_ONLY);
   
   -- 命名实例
   D ATTACH 'Driver={SQL Server};Server=192.168.1.100\INSTANCE;Database=mydb;UID=sa;PWD=password;' AS db2 (TYPE sqlserver_scanner, READ_ONLY);
   
   -- ODBC Driver 17 for SQL Server（如果已安装）
   D ATTACH 'Driver={ODBC Driver 17 for SQL Server};Server=192.168.1.100;Database=mydb;UID=sa;PWD=password;' AS db3 (TYPE sqlserver_scanner, READ_ONLY);
   ```

**注意：** 
- 命名实例不需要指定端口号，ODBC 会自动解析
- Windows 上不要添加 `TDS_Version` 或 `ClientCharset` 参数

### 跨平台连接字符串

如果需要在两个平台上使用相同的连接字符串，推荐使用 DSN 配置：

1. **配置 DSN**（`/etc/odbc.ini` on Linux，Windows ODBC 数据源管理器）：
   ```ini
   [mydb]
   Driver=FreeTDS    # Linux
   # Driver=SQL Server  # Windows
   Server=192.168.1.100
   Port=1433
   Database=mydb
   UID=sa
   PWD=password
   ```

2. **使用 DSN 连接**：
   ```sql
   D ATTACH 'DSN=mydb;' AS mydb (TYPE sqlserver_scanner, READ_ONLY);
   ```

## CI/CD 集成

### GitHub Actions 示例

```yaml
name: Build

on: [push, pull_request]

jobs:
  build-linux:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
        with:
          submodules: recursive
      
      - name: Install system dependencies
        run: |
          sudo apt update
          sudo apt install -y freetds-dev build-essential cmake git
      
      - name: Setup vcpkg
        run: |
          git clone https://github.com/microsoft/vcpkg.git ../vcpkg
          ../vcpkg/bootstrap-vcpkg.sh
      
      - name: Build
        run: make release
      
      - name: Test
        run: make test

  build-windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v3
        with:
          submodules: recursive
      
      - name: Setup vcpkg
        run: |
          git clone https://github.com/microsoft/vcpkg.git ..\vcpkg
          ..\vcpkg\bootstrap-vcpkg.bat
      
      - name: Build
        run: make release
      
      - name: Test
        run: make test
```

## 最佳实践

1. **使用版本控制的 vcpkg**：将 vcpkg commit hash 固定，确保团队使用相同版本
   ```bash
   cd vcpkg
   git checkout <commit-hash>
   ```

2. **清理构建缓存**：定期清理以避免缓存问题
   ```bash
   make clean
   rm -rf vcpkg_installed/
   ```

3. **使用 Ninja 生成器**：在大型项目上更快
   ```bash
   GEN=ninja make release
   ```

4. **启用 ccache**：加速重复构建
   ```bash
   export CMAKE_CXX_COMPILER_LAUNCHER=ccache
   make release
   ```

5. **检查依赖更新**：定期更新 vcpkg 以获取安全补丁
   ```bash
   cd vcpkg
   git pull
   ./bootstrap-vcpkg.sh
   ```

## 更多资源

- [DuckDB 扩展开发文档](https://duckdb.org/docs/extensions/overview)
- [vcpkg 官方文档](https://vcpkg.io/en/getting-started.html)
- [FreeTDS 官方文档](https://www.freetds.org/userguide/)
- [unixODBC 官方文档](http://www.unixodbc.org/)

## 获取帮助

如果遇到问题：
1. 运行 `make check-deps` 检查依赖
2. 查看本文档的故障排除部分
3. 在项目 GitHub 仓库提交 issue
