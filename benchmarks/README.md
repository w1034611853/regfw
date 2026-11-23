# regfw 性能复测入口

## 配对测量

[本轮实现与测量](../docs/Performance-Compact-Dispatch.md) 使用真实 RegisterBank 的
`dispatch_layout.cpp`，覆盖多个 View、无关/局部 Reserve、普通/高位稀疏地址及九份实体的
多个大数组。`poe_init.cpp` 测仓库内 POE 形状的静态 PF/PF_VF/VF 拓扑，分别记录构造、
finish、Summary/Full 格式化及内存；它不代表私有 QEMU/POE 设备的端到端耗时。

先用相同 Release 配置分别构建两个 SDK 的库，再用当前目录的同份基准构建消费者：

```sh
python3 benchmarks/build_layout_study.py --sdk /path/to/before \
  --library /path/to/before-build/libregfw.a --out /tmp/study-before
python3 benchmarks/build_layout_study.py --sdk /path/to/after \
  --library /path/to/after-build/libregfw.a --out /tmp/study-after
# 等待全部构建、功能测试和 sanitizer 退出后再计时。
python3 benchmarks/measure_layout_study.py --base /tmp/study-before \
  --candidate /tmp/study-after --out /tmp/study-results \
  --blocks 4 --iterations 3000000 --cpu 1
```

构建脚本需要两个匹配版本的 Git 检出及恢复后的 hlog；输出目录须尚不存在。
`--groups` 可选择 scalar、mapped、dispatch、memory、init、handles、errors；`--cases` 用于
单项复测。输出保存版本、库与二进制摘要、逐次数据及数值校验。
共享表统计、`mallinfo2` 分配量及进程峰值 RSS 分别报告。

同步架构精简增加了错误请求基准。用 `build_layout_study.py --names errors` 构建
两个版本的 `mmio_errors.cpp`，再对配套二进制运行：

```sh
python3 benchmarks/measure_layout_study.py --base /tmp/study-before \
  --candidate /tmp/study-after --out /tmp/error-results \
  --blocks 5 --iterations 400000 --cpu 1 --groups errors
```

四种探针覆盖已配置 View 的空洞、映射地址空间之外、自然对齐失败和非法访问宽度。
读写分别测量，并核对返回状态、无效读值和寄存器状态。配对方法见
[同步架构精简报告](../docs/Performance-Architecture-Sync.md)。

## pre_access / 地址分发对照

`mmio.cpp` 支持 `REGFW_BENCH_PRE_ACCESS=none|global|view|both`。钩子执行可观察的计数操作；三个手写参照均先执行钩子，再做地址解码与 switch 分发。`switch_raw` / `switch_normalized` 保留简单解码参照；`range_switch` 使用 if/else 范围链再进入 1000-case switch，`REGFW_BENCH_RANGES=1|10|100` 选择每个寄存器区的范围数（混合布局另外经过 8 个 VF 范围）。编译器可以正常优化范围链。正确性对照检查有效 1/2/4 字节访问的寄存器值、读响应、回调效果和钩子次数。分别编译两个版本时必须使用**同一份新的 mmio.cpp**，各自配套版本的头文件和库，保持优化等级一致。

手写参照的 pre_access 位于解码之前；regfw 沿用有效解析后的钩子语义。本基准只测有效请求，不能用于证明非法/被拒绝访问的钩子时序一致。没有运行真实 QEMU dispatch，也没有假定这些合成范围等于某个设备的实际范围分布。

```sh
# 两个版本先按相同 Release 配置构建 libregfw.a。
# 例如用新的基准源文件链接旧版库：
g++ -O3 -DNDEBUG -std=c++20 -fno-strict-aliasing \
  -I"$OLD/include" -I"$OLD/third_party/hlog/include" \
  "$NEW/benchmarks/mmio.cpp" "$OLD_BUILD/libregfw.a" \
  -L"$OLD/third_party/hlog/lib" -Wl,-rpath,"$OLD/third_party/hlog/lib" \
  -lhlog -pthread -o "$RESULTS/baseline"

# candidate 也用同一份新 mmio.cpp 链接配套候选库；若候选已有新基准源文件，
# 可使用 REGFW_BUILD_BENCHMARKS=ON 的 bench_mmio_dynamic。
python3 benchmarks/measure_hooks.py \
  --baseline "$RESULTS/baseline" --candidate "$NEW_BUILD/bench_mmio_dynamic" \
  --out "$RESULTS/hooks" --blocks 3 --passes 5
```

输出目录须为空且放在 SDK 之外。Linux/glibc，固定 CPU、随机场景顺序、版本交替；每进程先启动/退出一个线程以使用多线程引用计数路径，访问本身串行。96 场景涵盖四种钩子组合、普通/30% 映射/纯映射、有无寄存器回调、读写、未绑定 logger/INFO logger。输出环境与二进制摘要、原始采样和汇总；不会把微基准结果当成真实 QEMU 整机收益。

拆分 identity / 单下标 converter 时，将 `mmio.cpp` 分别以 `-DREGFW_BENCH_SINGLE_LAYOUT=1 -DREGFW_BENCH_IDENTITY=0或1 -DREGFW_BENCH_INDEX=0或1` 编译，生成 `baseline-0-0`、`baseline-0-1`、`baseline-1-0`、`baseline-1-1`，候选版本同样生成四个二进制。关闭功能会从声明/布局中移除对应映射，不是绑定一个返回原值的 resolver。固定 8 VF 布局和同一随机地址流，手写参照也关闭相同功能：

```sh
python3 benchmarks/measure_routes.py --baseline-prefix "$RESULTS/baseline" \
  --candidate-prefix "$RESULTS/candidate" --out "$RESULTS/routes"
```

默认测量包含 24 个场景：四种映射组合的无钩子/全局钩子读写（10 范围），以及双映射下的 1/100 范围敏感性对照。CPU/引用计数/进程配对规则与 `measure_hooks.py` 相同。不同功能组合的差额包含执行路径变化，不能当作严格可相加的单个函数耗时。可用 `--pairs 1-0 1-1 --ranges 10` 仅测 identity-only / 双转换的 8 个场景；两种 prefix 指向同一组二进制即可做 A/A 对照。

要测 index resolver 签名的影响，在同一版本上把上述 `0-1` 和 `1-1` 各编译两次。`-DREGFW_BENCH_LIGHT_INDEX=0` 使用完整 `RegAccessInfo`，`-DREGFW_BENCH_LIGHT_INDEX=1` 使用 VF 和逻辑下标两个参数。两者共用路由表、范围校验和读写校验。分开保存为 `full-0-1`、`light-0-1` 等，再运行：

```sh
python3 benchmarks/measure_routes.py --baseline-prefix "$RESULTS/full" \
  --candidate-prefix "$RESULTS/light" --pairs 0-1 1-1 --ranges 10 \
  --blocks 5 --passes 5 --out "$RESULTS/index-signature"
```

## 分发查表与采样

查表优化、剩余差距定位及编译成本见 [Performance-Dispatch.md](../docs/Performance-Dispatch.md)。`measure_routes.py --ranges 10` 测全部四种转换组合的 16 个读写/hook 场景。

Linux x86-64 可把 `sample_ips.cpp` 链接进单布局基准，并用 `-no-pie` 生成诊断二进制。设置 `REGFW_SAMPLE_IPS=/absolute/samples.txt` 后收集 IP，使用 `python3 benchmarks/summarize_ips.py BINARY SAMPLES` 按符号汇总。采样运行会扰动时序，正式性能比较不链接/启用它。

## 编译和初始化

```sh
make -f build.mk bench BUILD=build-bench JOBS=2
```

`measure_formula.py` 汇总编译耗时、峰值 RSS、初始化与 MMIO。

头文件入口成本矩阵：

```sh
python3 benchmarks/measure_headers.py --record /absolute/header-results \
  --sdk /absolute/regfw --cxx g++ --tus 8
```

对同一声明负载分别测量轻量入口 `regfw/bundle.hpp` 与完整入口 `regfw/regfw.hpp` 的单个消费 TU、各自预编译设备头（PCH）后的 TU、以及 8 个 TU 的并行 clean build；记录 PCH 自身构建成本并校验编译器确实使用了 PCH。

## 单线程 RegView 与 MMIO 优化

- `mmio_scalar.cpp` / `measure_mmio_scalar.py`：直接 Bundle/AddressSpace、按需 info、完整复制、logger、finish 及表大小对照，见 [Performance-MMIO-Scalar](../docs/Performance-MMIO-Scalar.md)。
- `handles.cpp` / `measure_handles.py`：原生指针、`.at(i).bits`、`.at(i).ref()` 对照；构建和配对命令见 [Performance-Handles](../docs/Performance-Handles.md)。
- `measure_hooks.py` / `measure_routes.py`：pre_access 与 identity/index 组合，见 [Performance-Hooks](../docs/Performance-Hooks.md)。
- `sample_ips.cpp` / `summarize_ips.py`：可选 Linux x86_64 IP 采样，诊断结果见 [Performance-Dispatch](../docs/Performance-Dispatch.md)；正式计时不链接采样器。
- `snapshot_stream.cpp`：9 实例×2048 独立 u32，或 9×65536 u32 数组，真实 bank Writer/Reader 内存流及恢复校验。编译需 `-Iextras -Itests/snapshot_backend`，仅基准使用测试后端头；命令与结果见 [固定生命周期与快照测量](../docs/Performance-Fixed-Lifetime.md)。
