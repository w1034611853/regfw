# 工程接入

[返回 README](../README.md)

## CMake 子项目

把发布包中的 SDK 目录放到项目的 `third_party/regfw/` 后：

```cmake
add_subdirectory(third_party/regfw)
add_executable(device main.cpp)
target_link_libraries(device PRIVATE regfw::regfw)
```

默认使用 SDK 自带的 `third_party/hlog`。若整个项目已有 hlog，配置时用 `-DHLog_ROOT=/absolute/path/to/hlog`；同一进程应共用一份 runtime。引入父工程已定义的 `HLog::HLog` 目标也受支持。

`regfw::regfw` 传播 C++20、公共头目录及 GNU/Clang 的 `-fno-strict-aliasing`（原生 union 的 raw/bitfield 访问需要）。用户自定义寄存器组的构造实现属于设备代码，仍应加到设备 target 中。可用 `regfw_precompile_bundle(device, "device_regs.hpp")` 启用提供的 PCH 辅助函数（由 bundle_pch 测试端到端覆盖）。设备声明头建议使用 `REG_LAYOUT_DECL` 并包含 `<regfw/bundle.hpp>`，把大布局实现留在包含完整入口的 .cpp，这样 PCH 缓存的头部本身不再携带 constexpr 布局编译器。

只构建公开示例、不编测试程序：

```sh
cmake -S . -B examples-build -DREGFW_BUILD_TESTS=OFF -DREGFW_BUILD_EXAMPLES=ON
cmake --build examples-build --parallel 2
./examples-build/regfw_quickstart
./examples-build/regfw_pf_vf --layout
```

这些命令在 SDK 根目录执行。详细依赖见 [README](../README.md)。

## 安装后的独立消费者

先在 SDK 根目录执行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
cmake --install build --prefix /tmp/regfw-install
```

消费者工程使用：

```cmake
find_package(regfw CONFIG REQUIRED)
add_executable(device main.cpp)
target_link_libraries(device PRIVATE regfw::regfw)
```

配置消费者时传入 `-DCMAKE_PREFIX_PATH=/tmp/regfw-install -DHLog_ROOT=/absolute/path/to/hlog`。regfw 的 install 只安装自身，不把 bundled hlog runtime 安装到全局系统目录。搬移安装前缀后仍可使用，发布验证会实际测试这一点。

## hlog 与布局日志

框架公共头只依赖 `hlog.h`，不依赖 fmt。可把已有 `HLogLogger*` 传给 `reg::space(...).logger(handle)`，或用 hlog C++ `Logger` 的重载。logger 在 bank 使用期间应保持有效。

使用 `hlog.hpp` 格式化日志的设备 target 另行 `find_package(fmt CONFIG REQUIRED)` 并链接 `fmt::fmt`。配套 hlog 3.2.1 支持位域参数和 u8/u16/u32/u64 buffer 宏；其使用说明在 hlog SDK 的 README 中。

框架与设备共用 logger，等级通过 hlog 控制；不缓存另一份等级。`finish()` 的 Debug/Trace 输出详见 [诊断](Diagnostics.md)。

## QEMU C 边界

`include/regfw/qemu_bridge.h` 不依赖 QEMU 头。C++ 侧 `reg::make_qemu_bridge(bank.map())` 返回 `regfw_mmio_bridge`：opaque 指向 map，read/write 接收地址、值与 size，返回 QEMU MemTxResult 位。它是非拥有接口，bank 必须活得更久。

设备在自己的 read_with_attrs/write_with_attrs 适配层调用 bridge。若 QEMU 给的是窗口内偏移，先加该窗口在整个 map 中的基址；例如 VF i 的偏移 off 转为 `16 MiB + i*2 MiB + off`。不要重复加基址，也不要把已经是全局 map 偏移的地址再换算一次。

本包验证覆盖 C 边界和独立 C++ 消费者；真实 QEMU 进程的集成仍由设备工程验证。

## 原有字段式模块快照

完整寄存器快照沿用 hac::snapshot::Writer/Reader 与 module_var_save/load，支持 bank.save(file)/load(file) 和 VMSTATE_FIELDS(bank,...)。不要求使用 VMStateDescription；参见 [Snapshot](Snapshot.md)。MMIO bridge 不自动登记快照，模块仍从既有的保存/加载入口调用。
