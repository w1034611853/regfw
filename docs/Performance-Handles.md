# 0.37 单线程原生句柄优化

本分支从无异步的 `cec42e1f3a25bd8804129ff4c349a0dd54128742` 派生，面向单线程、固定生命周期的 QEMU SysBus 设备。没有合并 main，不包含异步队列、执行器或端点。此前 MMIO 钩子及分发表优化也已经回移；其测量记录见 [钩子报告](Performance-Hooks.md) 和 [查表报告](Performance-Dispatch.md)。

## 改动

- RegView 的 weak_ptr 改成普通引用计数。计数的复制/释放内联，热路径不再调用 weak_ptr 原子加减；Bank 销毁后仍能检测过期句柄。布局描述和 Bundle 的冷路径继续保留原 weak_ptr 接口。
- 视图建立时缓存稳定的原生存储指针和借用标志地址。子视图直接从父视图按元素偏移定位，不再重新查询虚拟存储接口及校验不可变的类型/布局。
- 一维 `.at()` 直接检查视图范围；`.at()`、`.ref()` 的轻量检查内联，错误处理保留在编译库中。多维坐标和 typed element index 仍校验形状。
- `.at()`、`.at_element()`、`.subspan()`、`.data()`、`.ref()`、原生字段、回调和完整 RegAccessInfo 的前端 API 不变。

本分支的所有同 Bank 句柄操作（包括复制/析构）与 Bank 生命周期必须串行化。此约束对应当前设备部署，不为并发句柄管理增加原子操作。已返回的原生引用仍遵循 C++ 对象生命周期规则。

## 测量

GCC 14.2、Linux x86_64、Xeon Platinum 8573C，`-O3 -DNDEBUG -fno-strict-aliasing`。固定 CPU 0，使用线程 CPU 时钟，5 轮交替进程，每项每轮 2000 万次，取跨进程中位数。计时期间没有编译或测试任务。

负载是实际 SDK 的 256 元素原生 union REG：动态下标 `(i * 13) & 255`，分别读位域和递增位域；每轮使用相同编译器屏障阻止消除内存访问。对照直接访问同一 Bank 的原生数组；每个读/写场景的校验和、版本间校验和均相同。没有 MMIO、hook 或 index resolver。

基线是此前 MMIO 优化回移后的 0.37（其原生句柄实现仍与 `cec42e1` 相同）；LTO 基线直接使用 `cec42e1`。所有二进制的头文件、库和对应优化级别配套。中间控制版本只替换普通生命周期计数并内联复制/析构，尚未缓存存储指针或简化重复检查。它的收益不能全部归因于单条原子指令。

| 构建 / 路径 | 旧句柄 ns | 普通计数＋内联复制/析构 ns | 完整优化 ns |
|---|---:|---:|---:|
| Release / `.at(i).bits` 读 | 25.46 | 17.17 | 1.26 |
| Release / `.at(i).bits` 读改写 | 25.36 | 17.38 | 1.59 |
| Release / `.at(i).ref()` 读 | 30.92 | 25.10 | 2.28 |
| Release / `.at(i).ref()` 读改写 | 30.49 | 25.12 | 2.71 |
| LTO / `.at(i).bits` 读 | 22.14 | 未测 | 1.72 |
| LTO / `.at(i).bits` 读改写 | 22.46 | 未测 | 1.55 |
| LTO / `.at(i).ref()` 读 | 24.34 | 未测 | 2.06 |
| LTO / `.at(i).ref()` 读改写 | 24.98 | 未测 | 2.49 |

同一 Release 候选里的裸数组读 / 读改写分别为 **0.37 / 0.64 ns**；`.at().bits` 仍分别增加约 **0.89 / 0.94 ns**。因此这不是零成本，也不能声称已与裸访问完全等速。LTO 候选裸数组为 0.65 / 0.63 ns，显示这个极短循环也受代码生成影响；当前优化不要求 LTO。

所有进程均报告 `libc_single_threaded=0`，旧 weak_ptr 实际走原子计数分支，虽然基准本身没有并发访问 Bank。这里保留了 QEMU 等多线程宿主可能具有的 libstdc++ 计数路径；优化后的句柄本身仅支持串行操作。Release 和 LTO 候选的六个测量函数均未出现 LOCK 指令。

同一候选二进制的 5 轮 A/A 中，`.at().bits` 读 / 写差异分别为 +1.09% / -0.17%；`.at().ref()` 为 -0.92% / -2.17%。

用户真实环境的 1.99 / 4.33 ns 使用不同的代码、硬件和构建条件，本页不声称复现了这些绝对值或 QEMU 整机加速比。此处是寄存器原生句柄路径，不能用来替代此前 MMIO 分发对照。


MMIO 对照使用 identity + index、10 范围、有/无全局 pre_access，3 轮交替进程、每进程 3 passes（每 pass 8 轮，去首轮）。基线包含此前分发表优化，候选再叠加本次句柄改动；每版使用匹配头文件重新编译基准。

| hook | 操作 | 改前 ns | 改后 ns | 变化 |
|---|---|---:|---:|---:|
| global | 读 | 106.46 | 108.44 | +1.86% |
| global | 写 | 104.09 | 102.16 | -1.85% |
| none | 读 | 101.16 | 102.47 | +1.29% |
| none | 写 | 95.21 | 95.85 | +0.67% |

四项变化在 ±1.9% 内，校验和一致；这里只作回归对照，不将这种幅度宣称为新的 MMIO 提速。原生句柄的收益主要发生在设备代码自身调用 `.at()` 的路径。

## 复现

使用 GCC 的 Linux 构建示例；每个版本的头文件和静态库必须匹配。先完成全部编译，再运行基准，不要在计时期间构建。

```sh
cmake -S /path/to/sdk -B /path/to/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/build --parallel 2
g++ -std=c++20 -O3 -DNDEBUG -fno-strict-aliasing \
  -I/path/to/sdk/include -I/path/to/sdk/third_party/hlog/include \
  /path/to/candidate/benchmarks/handles.cpp /path/to/build/libregfw.a \
  -L/path/to/sdk/third_party/hlog/lib -Wl,-rpath,/path/to/sdk/third_party/hlog/lib \
  -lhlog -pthread -o /path/to/handles
python3 benchmarks/measure_handles.py --baseline /path/to/before \
  --candidate /path/to/after --out /path/to/new-results --cpu 0
```

LTO 对照需要库编译时设置 `-DCMAKE_CXX_FLAGS=-flto`，基准编译/最终链接时也加 `-flto`。未启用 LTO 的库不能用仅最终链接 `-flto` 代替。可选 `--control` 指定只替换计数的中间版本；同一二进制传给 baseline/candidate 可运行 A/A。

## 更新现有设备

检出 `codex/regfw-037-single-thread-fastpath-20261117` 的固定提交；清理旧构建输出，使用该提交配套的头文件和库重编译 regfw 与 QEMU 设备。无需改变设备侧 `.at()`、hook 或 layout 声明。新旧二进制 ABI 不兼容，不能只替换 `.a` 或只拷贝头文件。

## 验证

- 严格 Release（`-Wall -Wextra -Werror`）功能测试通过，文档补齐后文档检查通过，共 35 项。
- ASan + UBSan：35/35 通过，`detect_leaks=1:halt_on_error=1`，包括泄漏检查。
- 新增原生 union、一维切片、多维坐标、元素复制/移动/赋值、Bank 移动及保存句柄失效回归；已有快照、宽值、动态 resolver、hook 和完整 info 用例继续通过。
- clang-format 17、生成文档/本地链接检查、Python 脚本编译检查通过。
- 安装后独立三 TU 消费者通过 `find_package(regfw 0.37.0 EXACT)` 构建并运行。
- 源码及 CMake 未包含 `AsyncExecutor`、`RegisterEndpoint`、`access_admission_`、`src/runtime/async.cpp` 或 `include/regfw/async.hpp`。
