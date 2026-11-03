# Buffer / Hexdump（3.2.0）

每行固定展示最多 16 字节，包含完整的时间、等级、logger、线程和函数前缀。
偏移始终按字节计数；同一次 dump 的行与其他 hlog 写入串行化。普通文件输出、
控制台镜像、显式 console 输出使用同一个行格式化器。

## C++：数组、span、vector

```cpp
#include "hlog.hpp"
#include <array>
#include <span> // 只有这个示例需要 C++20；其余宏支持 C++17

hlog::Logger logger("dma", HLOG_LEVEL_DEBUG, "0");
std::array<uint8_t, 35> buffer{};

HLOG_HEXDUMP_DEBUG(logger, hlog::u8,  buffer);
HLOG_HEXDUMP_DEBUG(logger, hlog::u16, buffer);
HLOG_HEXDUMP_DEBUG(logger, hlog::u32, buffer);
HLOG_HEXDUMP_DEBUG(logger, hlog::u64, buffer);
HLOG_HEXDUMP_DEBUG(logger, hlog::u32, std::span(buffer).subspan(1, 19));

// 两参数形式使用进程默认 logger，等级仍由默认 logger 的配置控制。
HLOG_HEXDUMP_DEBUG(hlog::u32, buffer);

// 裸指针必须显式传入字节数。
HLOG_HEXDUMP_DEBUG(logger, hlog::u32, buffer.data(), buffer.size());
```

支持 C 数组、`std::array`、`std::vector`、`std::span` 等具有连续 `data()` 和
`size()` 的容器。数组元素不必是字节，例如 `uint32_t words[8]` 自动传入 32
字节，`std::span<uint64_t>` 自动按元素大小计算总字节数；格式参数只控制展示
分组，不改变要打印的数据长度。宏同样接受原生 `HLogLogger*`。

三参数容器形式拒绝裸指针，避免把指针大小当成 buffer 长度；不支持
`std::vector<bool>`、非连续容器或非 trivially-copyable 元素。设备应在 buffer
有效、内容可安全读取期间调用，不会自动把大 buffer 复制到临时数组。

普通日志宏包括 `HLOG_HEXDUMP_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL`。
关闭对应等级后，不计算 format、buffer、pointer 或 length 表达式；logger
表达式只计算一次。

显式控制台版本为 `HLOG_CONSOLE_HEXDUMP_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL`，
参数形式相同，仅输出宿主 stderr，沿用显式 console 绕过文件等级的约定。

## 分组、字节序和尾部

| C++ 格式 | 每组字节数 | 每个完整行的分组数 | C 格式 |
|---|---:|---:|---|
| `hlog::u8` | 1 | 16 | `HLOG_DUMP_U8` |
| `hlog::u16` | 2 | 8 | `HLOG_DUMP_U16` |
| `hlog::u32` | 4 | 4 | `HLOG_DUMP_U32` |
| `hlog::u64` | 8 | 2 | `HLOG_DUMP_U64` |

多字节分组按**宿主机字节序**组成无符号整数。例如小端机器上的
`12 34 56 78`，u16 显示 `3412 7856`，u32 显示 `78563412`。
QEMU 的 guest 字节序不参与这里的自动转换；这里检查的是传入 buffer 的内存。

不足一组的尾部用 `??` 标出缺失字节。例如小端机器上的 `12 34 56` 按 u32
显示 `??563412`，按 u64 显示 `??????????563412`。不会越界读取、丢弃尾部
或补零伪造数据。非对齐起始地址也可以使用多字节格式。

所有格式的 ASCII 列位置一致；ASCII 按原始字节顺序显示。空 buffer 输出一行
带完整前缀的 `<empty>`。实际运行时输出见 [HEXDUMP_PREVIEW.txt](../examples/HEXDUMP_PREVIEW.txt)。

## C

C 宏需要 logger、格式、指针、**字节数**四个参数：

```c
#include "hlog.h"
uint32_t words[] = {0x11223344, 0x55667788};
HLOG_HEXDUMP_DEBUG(logger, HLOG_DUMP_U32, words, sizeof(words));
HLOG_CONSOLE_HEXDUMP_INFO(logger, HLOG_DUMP_U16, words, sizeof(words));
```

底层新增 `hlog_hexdump_ex(...)` / `hlog_console_hexdump_ex(...)`，最后一个
`uint32_t format` 参数为 1/2/4/8。其他值在启用的调用中返回
`HLOG_ERR_INVALID_ARGUMENT`，不产生部分输出。`data_len` 始终是字节数。

## Rust

Rust 接口接收 `&[u8]` / 字节数组 / `Vec<u8>`；多字节格式由参数控制：

```rust
#[hlog::named]
fn dump(logger: &hlog::SharedLogger, bytes: &[u8]) {
    hlog::lhexdump_debug!(logger, hlog::HexdumpFormat::U32, bytes);
    hlog::lhexdump_debug!(hlog::HexdumpFormat::U16, bytes); // 默认 logger
    hlog::lhexdump!(logger, hlog::LogLevel::Debug, hlog::HexdumpFormat::U64, bytes);
    hlog::lconsole_hexdump_info!(logger, hlog::HexdumpFormat::U32, bytes);
}
```

也提供 trace/info/warn/error/critical 的同形宏。

## 兼容和构建

原有 `HLOG_HEXDUMP(logger, level, data, byte_length)`、
`HLOG_CONSOLE_HEXDUMP(...)`、Rust `lhexdump!(logger, level, bytes)` 和 C ABI
`hlog_hexdump(...)` 均保留，默认 u8，并一并修复逐行前缀。

新格式使用 3.2.0 新增导出符号，`HLOG_ABI_VERSION=3` 和 SONAME
`libhlog.so.1` 保持不变。更新源码后先运行 `bash scripts/install.sh`，重新编译
使用新宏的设备模块；`verify_runtime.sh` 会拒绝缺少新符号的旧库。

C++ fmt 包装现在通过 `fmt::format_string` 在调用处检查格式字符串，可与
C++20/span 一起编译。直接调用格式化方法或 `QueryOutput::print` 时，动态
格式字符串使用 `fmt::runtime(text)`；通常的字面量及 HLOG 宏不需要改动。

```sh
bash tests/test_hexdump_headers.sh
python3 tests/test_hexdump_runtime.py ./lib/libhlog.so.1
```

完整 C++ 示例为 [cpp_hexdump.cpp](../examples/cpp_hexdump.cpp)，Rust 示例为
[hexdump.rs](../rust-client/examples/hexdump.rs)。
