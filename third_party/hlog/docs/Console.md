# HLog Console（runtime/client 3.2.0）

## 输出语义

| 调用 | 镜像关闭（默认） | 镜像开启 |
| --- | --- | --- |
| 普通 HLOG 日志、普通 hexdump | 文件 | 文件 + console 各一次 |
| 显式 console 宏、console hexdump | console 一次 | console 一次 |
| HMP Query 回调 output.print | 发起命令的 HMP | 发起命令的 HMP |

普通日志仍受 logger 等级过滤；显式 console 宏不受文件 logger 等级过滤，
即使 logger 是 Off 也能显式打印。直接调用通用 API 时 level=Off 不输出。
这些接口仍需要已加载的 runtime 和有效 logger，不是脱离 runtime 的 printf。

Console 目的地固定为宿主进程 **stderr**。如果 stderr 和 Guest 串口均在
同一个宿主 terminal 显示，两者能在同屏看到，但不是同一数据通道。
这里不会向 Guest UART RX 注入字节，也不会自动连接 `-serial tcp/pty`
的独立串口窗口。独立串口窗口需要另行实现 QEMU chardev 适配。
不要把 stderr 合并进 QMP JSON 通道。

## HMP/QMP

```text
(qemu) hlog console
HLOG console mirror: off (stderr)
(qemu) hlog console on
HLOG console mirror: on (stderr)
(qemu) hlog console off
HLOG console mirror: off (stderr)
```

`hlog console show` 同样查询状态。`hlog show` 也显示当前状态。
开关作用于现有、未来、重命名后的 logger，文件重新配置/轮转时保持。
只镜像开启后新产生的日志，不回放日志文件；重启进程恢复默认关闭。

```json
{"execute":"hlog","arguments":{"action":"console","target":"on"}}
```

将 `target` 换成 `off`/`show`，或者省略 target 即可。非法参数不会改变开关。
开关不修改 logger 等级；需要普通 Debug 镜像时还要设置对应模块 Debug。

## 显式打印接口

C（printf 风格，logger 是 `HLogLogger *`）：

```c
HLOG_CONSOLE_INFO(logger, "vf=%u l_pool=%u", vf, l_pool);
HLOG_CONSOLE_ERROR(logger, "DMA failed: addr=0x%llx", (unsigned long long)addr);
HLOG_CONSOLE_HEXDUMP(logger, HLOG_LEVEL_DEBUG, data, length);
```

C++17（fmt 风格，logger 是 `hlog::Logger`，沿用工程内 spdlog bundled fmt）：

```cpp
HLOG_CONSOLE_INFO(logger, "vf={} l_pool={}", vf, l_pool);
HLOG_CONSOLE_WARN(logger, "{:<8} {:>8}", "L_POOL", "VL_POOL");
HLOG_CONSOLE_ERROR(logger, "DMA failed: addr=0x{:x}", addr);
HLOG_CONSOLE_HEXDUMP(logger, HLOG_LEVEL_DEBUG, packet.data(), packet.size());
```

完整宏名：

```text
HLOG_CONSOLE_TRACE
HLOG_CONSOLE_DEBUG
HLOG_CONSOLE_INFO
HLOG_CONSOLE_WARN
HLOG_CONSOLE_ERROR
HLOG_CONSOLE_CRITICAL
HLOG_CONSOLE_HEXDUMP
```

Rust（与旧宏一样，函数使用 `#[function_name::named]`）：

```rust
hlog::lconsole_info!(logger, "vf={} l_pool={}", vf, l_pool);
hlog::lconsole_error!(logger, "DMA failed: addr=0x{:x}", addr);
hlog::lconsole_hexdump!(logger, hlog::LogLevel::Debug, packet);
```

完整宏名是 `lconsole_trace!`、`lconsole_debug!`、`lconsole_info!`、
`lconsole_warn!`、`lconsole_error!`、`lconsole_critical!`、`lconsole_hexdump!`。
可运行示例位于 `rust-client/examples/console.rs`。

C++ 格式化仍可能抛异常，Rust 自定义 Display 仍可能 panic；console 宏
不承担异常捕获职责。跨 QOM/C ABI 的边界仍需按项目规范处理。

## 格式及颜色

格式沿用文件日志，额外加 console 专用 `[HLOG]` 前缀：

```text
[HLOG] [time.millisecond] [level] [module/instance] [tid=1234] [function] message
```

上面是字段示意；实际时间格式、等级拼写与现有文件 formatter 完全一致。
镜像复用同一个 spdlog record，因此两端时间、线程号、名称、正文一致。
函数名为空时，和普通日志一样不显示函数字段。

| 等级 | 颜色 |
| --- | --- |
| Trace | 灰色 |
| Debug | 青色 |
| Info | 绿色 |
| Warn | 黄色 |
| Error | 红色 |
| Critical | 粗体紫色 |

颜色自动判断 stderr 是否为 TTY。管道/文件重定向默认无 ANSI 码。
`NO_COLOR` 关闭自动着色；`HLOG_CONSOLE_COLOR=always/never/auto` 可显式覆盖。
每条记录结束会恢复 ANSI 属性。文件及压缩归档不增加 `[HLOG]` 或颜色码。

普通和显式 console 的 hexdump 均为每行 16 字节、完整日志前缀、字节偏移和
ASCII。3.2.0 增加 u8/u16/u32/u64 分组及数组/span 宏，例如
`HLOG_CONSOLE_HEXDUMP_DEBUG(logger, hlog::u32, buffer)`，详见
[Buffer / Hexdump](Hexdump.md)。同一次 dump 的行不会被其他 hlog 写入穿插。
整个多行消息一次提交，HLog 自己的线程之间不会逐行交错；与 Guest 串口
或其他直接写终端的程序共享窗口时，仍可能与这些外部输出交错。
Console 为同步输出，慢终端或大量报文 dump 可能拖慢模型，默认关闭镜像。

## 升级和 QEMU 接入

1. 替换本包源码，运行 `scripts/install.sh` 安装新版 runtime。
2. 运行 `scripts/verify_runtime.sh lib/libhlog.so.1` 检查新符号。
3. 替换 QEMU 内的 loader .c/.h。
4. 使用 `qemu/hmp-query-integration.snippet` 更新 HMP 命令项。
5. 用 `qemu/hmp-hlog.c.inc` 的实现替换旧 `hmp_hlog()`；可复制内容到
   `monitor/hmp-cmds.c`，或将 inc 放到该目录后 include。不要保留两个定义。
6. 重编译 QEMU 和使用新宏的模型，完全退出并重新启动 QEMU。

原有两阶段 preload/apply-config、CMake 增量构建、安装脚本 jobserver
清理均保留，不需要新增 QEMU 命令行参数。

这是 ABI v3 的增量扩展，没有更改旧结构体。旧 client 可以使用新 runtime。
新 console 宏必须配新 runtime；新 loader 若遇到没有 console 符号的旧
ABI v3 runtime，保留旧 show/set/query 功能，但 console 命令提示升级。

验证范围和本地测试步骤见 [tests/README.md](../tests/README.md)。
