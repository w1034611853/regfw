# regfw 0.36.0 性能复测入口

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
