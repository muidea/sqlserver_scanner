# AGENTS.md - Development Guide for odbc-scanner

This document provides guidance for AI agents working on the odbc-scanner DuckDB extension.

## Project Overview

odbc-scanner is a DuckDB extension that provides ODBC connectivity, allowing DuckDB to query external databases via ODBC drivers. It implements table functions (`odbc_scan`, `odbc_query`) that connect to ODBC data sources.

## Technology Stack

- **Language**: C++ (C++17)
- **Build System**: CMake + Makefile (uses DuckDB extension template)
- **Dependencies**: DuckDB (submodule), ODBC libraries, vcpkg
- **Formatting**: clang-format (LLVM base style with customizations)

## Build Commands

```bash
# Build release version (default)
make release

# Build debug version
make debug

# Build with assertions
make relassert

# Build release with debug info
make reldebug

# Clean build artifacts
make clean

# Update DuckDB submodule
make update

# Format code (apply clang-format)
make format

# Check formatting without applying
make format-check

# Run clang-tidy for static analysis
make tidy-check
```

## Test Commands

```bash
# Run all tests (default - runs release tests)
make test

# Run tests for specific build types
make test_release    # Test release build
make test_debug      # Test debug build
make test_reldebug   # Test relwithdebinfo build

# Run a single test (using unittest pattern)
./build/release/test/unittest "test_name_pattern"

# Examples of single test patterns:
./build/release/test/unittest "test/odbc*"          # All ODBC tests
./build/release/test/unittest "test/odbc_scan"      # Specific test
./build/release/test/unittest "*connection*"        # Tests with "connection" in name

# Skip tests (for CI outside docker)
SKIP_TESTS=1 make release
```

## Code Style Guidelines

### Formatting

- **Format Tool**: clang-format (configuration in `.clang-format`)
- **Check formatting**: `make format-check`
- **Apply formatting**: `make format` or `make format-fix`
- **Configuration**: Uses LLVM base style with customizations:
  - Tab width: 4
  - Column limit: 120
  - Pointer alignment: Right (`char* ptr`)
  - Use tabs for indentation
  - Sort includes: Regroup

### Naming Conventions

- **Classes**: PascalCase (e.g., `ODBCScanFunction`, `ODBCConnection`)
- **Functions**: PascalCase (e.g., `ParseConnectionParameters`)
- **Variables**: camelCase (e.g., `connectionString`, `result`)
- **Member variables**: camelCase or underscore-prefixed (e.g., `table` or `_table`)
- **Constants**: PascalCase or UPPER_SNAKE_CASE
- **Namespaces**: lowercase (e.g., `duckdb`)

### Code Structure

#### File Organization
- Header files in `src/include/`
- Implementation in `src/`
- Storage-related code in `src/storage/`
- Each header file should have DuckDB license header:
```cpp
//===----------------------------------------------------------------------===//
//                         DuckDB
//
// odbc_scanner.hpp
//
//
//===----------------------------------------------------------------------===//
```

#### Include Order (clang-format will reorganize)
1. Primary headers (duckdb.hpp)
2. Extension headers
3. Standard library
4. System headers

#### Header Guards
- Use `#pragma once` instead of traditional guards

### Error Handling

Use appropriate exception types from DuckDB:

- `throw InvalidInputException("message with %s", var)` - For invalid user input
- `throw IOException("message")` - For I/O errors
- `throw BinderException("message")` - For binding/parameter errors
- `throw NotImplementedException("message")` - For unimplemented features
- `throw InternalException("message")` - For internal errors
- `throw NotImplementedException("message")` - For serialization not supported

### Memory Management

- Use DuckDB smart pointers: `make_uniq<T>()`, `make_shared<T>()`
- Avoid raw `new`/`delete`
- Use `unique_ptr` and `shared_ptr` from DuckDB namespace

### Types

- Use DuckDB types: `idx_t` (unsigned index), `string`, `Value`, `Vector`, `DataChunk`
- Use `LogicalType` for type system
- Avoid C types where DuckDB equivalents exist

### Common Patterns

#### Table Function Implementation
```cpp
// Bind function
static unique_ptr<FunctionData> ODBCBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
    // Implementation
}

// Global state
struct ODBCGlobalState : public GlobalTableFunctionState {
    unique_ptr<ODBCResult> result;
    idx_t MaxThreads() const override { return 1; }
};

// Scan function
static void ODBCScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    // Implementation
}
```

#### ODBC Resource Management
```cpp
// Use custom deleters for ODBC handles
using ODBCStatementPtr = duckdb::unique_ptr<void, void (*)(SQLHSTMT)>;

inline void ODBCStatementDelete(SQLHSTMT stmt) {
    if (stmt) {
        SQLFreeStmt(stmt, SQL_CLOSE);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }
}
```

### Testing

- Tests should be in `test/` directory
- Use DuckDB's test framework
- Test patterns follow DuckDB conventions
- Tests require ODBC driver to be available

### Git/Development Workflow

- Commit messages should be descriptive
- Update DuckDB submodule with `make update`
- Format code before committing (`make format`)
- Build and test before submitting

### Key Files

- `CMakeLists.txt` - Main CMake configuration
- `Makefile` - Extension build targets
- `extension_config.cmake` - DuckDB extension config
- `.clang-format` - Code formatting rules
- `src/odbc_scanner.cpp` - Main table function implementations
- `src/include/odbc_scanner.hpp` - Main header with function definitions

### Agent Workflow Requirements

**VERY IMPORTANT**: When you have completed a task, you MUST run the lint and typecheck commands with Bash if they were provided to you to ensure your code is correct. If you are unable to find the correct command, ask the user for the command to run and if they supply it, proactively suggest writing it to AGENTS.md so that you will know to run it next time.

**NEVER commit changes unless the user explicitly asks you to**. It is VERY IMPORTANT to only commit when explicitly asked, otherwise the user will feel that you are being too proactive.

### Code References

When referencing specific functions or pieces of code include the pattern `file_path:line_number` to allow the user to easily navigate to the source code location.

<example>
user: Where are errors from the client handled?
assistant: Clients are marked as failed in the `connectToServer` function in src/services/process.ts:712.
</example>

### Important Notes

1. This is a DuckDB loadable extension - it links against DuckDB
2. The extension uses ODBC for external database connectivity
3. Build requires ODBC development libraries (`unixODBC-dev`)
4. Tests may be skipped outside of Docker environment
5. The DuckDB submodule must be initialized: `git submodule update --init --recursive`

## 数据库错误诊断基础规约

本扩展的错误处理须遵守 [federated-query-engine/AGENTS.md](../federated-query-engine/AGENTS.md) 中“数据库错误诊断基础规约”。公共结构化传输在主仓 `internal/sourceerrors/native/` 维护，使用 `scripts/sync_query_source_error.py` 同步/校验，禁止另建产品专用协议。必须区分真实源端诊断、驱动诊断缺失、适配器结果解码失败和本地能力拒绝，保留实际阶段和对象/类型上下文，不得猜测 SQLSTATE 或覆盖 DML/DDL 执行状态。该规范不扩大现有能力；原生修改与编译使用 `fedqueryBuilder`，禁止修改 vendor。
