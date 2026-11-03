# regfw 0.35.2 — Correctness Fix Release

## 范围

本版只修复 0.35.1 的 Formula Decoder **安装/校验**路径、动态 Table 描述去重及构建/打包脚本；不修改公开 REG DSL、MMIO 入口、事务、回调、resolver、pre_access、快照 Schema 或既有热路径策略。**不是新的性能优化版本。**

## 确认并修复的缺陷

| 级别 | 位置 | 问题 | 修复 |
|---|---|---|---|
| P0 | `src/address_view.cpp` `push_array()` | 不同动态 Table 仅凭 count/width/rank 等元数据被共享，忽略真实 offset/linear 记录 | Table 不进行不安全跨声明去重；每个 Table 绑定独立记录表，非 Table 仍按完整公式形状复用 |
| P0 | `src/formula.cpp` `install_dynamic()` | 同优先级 Mount 冲突判断将不同 Mount 的相对坐标直接比较，导致误报或漏判 | 比较统一为绝对 MMIO 地址，保留原来的有序区间归并语义 |
| P0 | `src/formula.cpp` `install_dynamic()` | 高优先级稀疏 Mount 的包围区间遮住两个真实同优先级低层冲突，布局被静默接受 | 增加安装期等优先级实际占用区间检查；只允许被更高优先级**真实 REG/Reserve 字节**完全遮蔽的低层冲突 |
| P1 | `src/formula.cpp` `FormulaInstaller` | Reserve 与稀疏数组包围区间相交即误报 `REGFW_ENTITY_SHADOWED` | 与动态端共用元素级真实占用检查；避免空洞误报，真正遮蔽仍保留 warning |
| P2 | `benchmarks/measure_formula.py` | Linux `/usr/bin/time -v` 第一行及 `Maximum` 大小写导致墙钟和 RSS 解析错误 | 使用 Python `perf_counter` 计时，按平台解析 RSS；compiler 类型按 `--version` 识别 |
| P2 | 交付系统 | 原包 macOS `._*` 资源元数据、缺失 `build.mk` | 新建 CMake 包装 Makefile、可复现的干净源码打包脚本；重新生成并验证 SHA256SUMS |

## 回归验证

- GCC 14.2（Debian x86_64）、C++20、Release、`-Wall -Wextra -Werror`，完整库+测试**成功构建**。
- `ctest --test-dir build --output-on-failure`：**34/34 PASS**，包含全部旧 CTest（33 项）与新增的 `formula_regressions`。
- 重新执行：`formula_diff`、`formula_golden`、`formula_fastpaths`、`address_view`、`snapshot`、`static_layout`、`symbolic_layout`、`layout_separate`、`bundle_pch`、`static_compile_fail` 与 `documentation`。
- 新回归场景：Table stride 300/304 无误复用、12+12 元素全部可读写、空洞不可命中、Table 快照往返；不同 Mount 真实重叠拒绝/互补稀疏接受；高优先级稀疏 View 空洞下的真实冲突必须拒绝；静态/动态 Reserve 空洞均不误报警告。
- GCC 14.2，Debug，ASan+UBSan（`ASAN_OPTIONS=detect_leaks=0`）：`address_view`、`symbolic_layout`、`formula_diff`、`formula_fastpaths`、`formula_regressions`、`snapshot`，**6/6 PASS**。
- Clang 17，Release，`-Wall -Wextra -Werror`：核心库构建及 `formula_regressions` **1/1 PASS**。
- 可选 benchmark 四个二进制已通过 GCC 14.2 严格编译；`bench_scale_formula` 与 `bench_init_formula` 实际运行成功，`bench_mmio_dynamic` / `bench_mmio_formula` 的 10k trace 正确性自检成功。该结果不代表已完成完整性能对照。
- 原有 0.35.1 的 Callback 热路径性能仍有已知差距，本版未进行 MMIO TPS 独立 A/B，也不声称性能已经达到旧 StaticWord 目标。

## 自行验证

```bash
# 在包根目录
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Werror'
cmake --build build -j2
ctest --test-dir build --output-on-failure

# 快速确认这次新增的回归
ctest --test-dir build -R 'formula_regressions|formula_diff|address_view|snapshot' --output-on-failure

# 不要求 ninja 的替代方案
make -f build.mk run-tests JOBS=2

# 验证打包文件的 SHA256（在包根目录）
sha256sum -c SHA256SUMS
```

**限制：** 本次没有真实 POE/DAG 工程和 QEMU 整机环境，未测试硬件业务侧 machine init、VF 路由及迁移集成，也未重新测量完整 MMIO TPS。当前版本适合作为开发集成与回归测试候选；生产升级需在目标 QEMU 环境完成验收。
