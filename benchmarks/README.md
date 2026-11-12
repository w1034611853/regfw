# regfw 性能复测入口

## pre_access / 地址分发对照

本文钩子优化报告以无异步的 `cec42e1f3a25bd8804129ff4c349a0dd54128742`（0.37.0）及回移同一优化的版本配对；PR 集成 main 0.38.0。复现时不要混用这两套版本。

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

该测量包含 24 个场景：四种映射组合的无钩子/全局钩子读写（10 范围），以及双映射下的 1/100 范围敏感性对照。CPU/引用计数/进程配对规则与 `measure_hooks.py` 相同。不同功能组合的差额包含执行路径变化，不能当作严格可相加的单个函数耗时。

## 历史完整复测入口

```bash
make -f build.mk bench BUILD=build-bench JOBS=2
# 或者显式运行（需先构建 bench_* 可执行文件）
REGFW_BENCH_BUILD=build-bench CXX=g++ python3 benchmarks/measure_formula.py
```

Linux 使用 `/usr/bin/time -v`，macOS 使用 `-l`；编译耗时使用 `time.perf_counter()` 实测，最大 RSS 从对应子进程的输出解析。0.36.0 尚未执行真实 QEMU 负载性能测试，旧版本报告仅供历史比较。

---

# 当前版本性能复测

只保留当前使用的基准和脚本，不随 SDK 打包历史 JSON、日志、二进制或中间报告。

Linux x86_64、GCC 13/C++20、Python 3、GNU Make、taskset：

```sh
python3 benchmarks/reproduce.py --baseline /absolute/regfw-0.32.0 \
  --out /absolute/empty-results --cmake cmake
```

输出目录必须在两个 SDK 之外且为空。脚本先构建，再固定 CPU 交替测量；测试普通/带回调、30% 映射/纯映射、logger 未绑定/已绑定 INFO 两种模式。初始化含大数组、宽寄存器、完整 DAG 以及 Summary/Full 格式化；编译测量包含 64/128/256 元素与单独布局实现的 `make -j32`。最后使用同一旧版二进制进行 A/A 对照，观察环境波动。生成的结果 JSON 和日志仅写入输出目录。

编译峰值 RSS 是单个编译进程树的 KiB；并行试验记录 cgroup 内存，包含环境和页缓存，二者不可混用。digest 排除地址相关的函数指针和 source_location，比较所有静态计划语义字段。微基准不等于 QEMU 整机性能。

脚本兼容 0.32.0 基线；不要混用两个版本的头文件和库。各项量化结果和限制见 [Performance](../docs/Performance.md)。

头文件入口成本矩阵（方向三）：

```sh
python3 benchmarks/measure_headers.py --record /absolute/header-results \
  --sdk /absolute/regfw-0.33.3 --cxx g++ --tus 8
```

对同一声明负载分别测量轻量入口 `regfw/bundle.hpp` 与完整入口 `regfw/regfw.hpp` 的单个消费 TU、各自预编译设备头（PCH）后的 TU、以及 8 个 TU 的并行 clean build；记录 PCH 自身构建成本并校验编译器确实使用了 PCH。

POE 型稀疏布局基准（0.33.3）：

```sh
# 快路径（默认）与强制完整构建器对照
for m in "" "-DPOE_FORCE_FULL=0"; do
  python3 benchmarks/measure_compile.py --record /absolute/poe$m -- \
    g++ -std=c++20 -O2 -DNDEBUG -fno-strict-aliasing \
    $m \
    -I/path/to/regfw/include -I/path/to/regfw/third_party/hlog/include \
    -c benchmarks/poe_layout.cpp -o /absolute/poe$m.o
done
```

约 2000 个寄存器声明、8 VF、PF-shared 在 17 个 business 视图中重挂载、32/64/128/512/2048-bit 混合、稀疏数组与双 ACC 地址族；无真实地址冲突。
