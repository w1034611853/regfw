# regfw 0.38.0 验证记录

即时寄存器状态与可选异步业务执行、并发 drain 修复的验证范围及复现命令见 [0.38.0 验证说明](docs/Validation-0.38.0.md)。以下内容为历史版本记录。

---

# regfw 0.37.0 验证记录

这是 **代码格式与内部目录重构**，用户 API、MMIO 执行行为和快照格式不作改动。

- GCC 14.2 Release `-Wall -Wextra -Werror` 构建通过，CTest **35/35** 通过。
- Clang 17 严格核心库构建通过，导出强定义符号 224/224 与 0.36.0 一致。
- 安装和独立 `find_package(regfw 0.37.0 EXACT)` 消费工程通过。
- `scripts/update_docs.py --check` 通过；新源码包需再独立解压复验。
- 详细结果见 [0.37.0 发布报告](RELEASE-REPORT-0.37.0.md)。

构建验证（GCC 14.2、Clang 17、CTest、安装消费）见 `docs/Architecture.zh-CN.md` 的验证说明和本次发布报告。

---

# regfw 0.36.0 验证记录（单遍原生布局生成 + 共享 EntityLayout Hash）

- GCC 14.2 Release `-Wall -Wextra -Werror`：完整构建，**CTest 35/35 通过**。
- Clang 17 Release `-Wall -Wextra -Werror`：核心库独立构建通过。
- 新增 `static_runtime_errors`：验证旧编译期建表的几何错误现在在初始化时正确拒绝；类型和声明层面的编译失败契约仍保留。
- 全部用户可见 DSL、访问执行与快照接口保持；实际跨 TU、PCH、Snapshot、Access Group、Identity、宽寄存器、Reserve/Overlap、Formula 差分与 Golden 测试保持通过。
- 安装与外部 CMake 消费：`find_package(regfw 0.36.0 EXACT REQUIRED CONFIG)` 通过，并已运行 MMIO 读写用例。
- 合成 POE 形状的 `-fsyntax-only`：0.36.0 成功 **15.30s / 1,001,836 KiB RSS**；0.35.2 在 GCC 默认 constexpr limit 下失败 **29.62s / 1,891,204 KiB RSS**。旧版失败，不得宣称两个成功构建的严格加速比。
- 合成规模测试：`bench_scale_formula` 单次运行构造 **13.770ms**、`finish` **0.022ms**、峰值 RSS **8,424 KiB**；`bench_init_formula dag 1` 构造约 **3.32ms**、`finish` 约 **0.089ms**。
- 未测真实 POE（用户报告约 4min 编译）、真实 10 模块 machine init、QEMU 迁移流/并发。宽数组按有效 4B lane 构造 Hash，安装成本需目标环境实测。

完整变更与限制见 [DELIVERY-REPORT-0.36.0.md](DELIVERY-REPORT-0.36.0.md)。

---

# regfw 0.35.2 验证记录（正确性修复）

- **Linux GCC 14.2、Release、`-Wall -Wextra -Werror`：完整构建成功，CTest 34/34 通过。**
- 新增 `formula_regressions`：动态 Table 不同地址记录禁止错误共享；动态同优先级真实物理字节重叠拒绝、互补稀疏数组接受；高优先级稀疏 View 空洞下隐藏的同优先级冲突拒绝；静态/动态 Reserve 空洞均不误报警告，有效 Reserve 遮蔽有警告；Table 地址访问、未映射空洞与快照往返。
- `formula_diff`、`formula_golden`、`formula_fastpaths`、`address_view`、`snapshot`、`static_layout`、`symbolic_layout`、分离 TU、PCH、编译失败契约和 documentation 均通过。
- Debug ASan+UBSan（GCC 14.2，`detect_leaks=0`）针对性 6/6 通过；Clang 17 Release+`-Werror` 核心库与新 `formula_regressions` 1/1 通过。
- 可选 benchmark 目标全部 GCC 14.2 Release `-Werror` 构建成功；scale/init 基准可运行，动态和静态 Formula MMIO 的 `PASSES=0` 正确性自检通过。
- 核心 API 和运行期 MMIO 热路径保持不变；本轮未重新测量 MMIO TPS，也没有真实 QEMU/POE/DAG 业务工程，不能声称整机验收通过。
- Build: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS='-Wall -Wextra -Werror' && cmake --build build -j2 && ctest --test-dir build --output-on-failure`。

---

# regfw 0.35.1 验证记录（性能收敛与 pattern 化）

- 本机：macOS arm64（Apple M），GCC 16.2.0（Homebrew，`-isysroot $(xcrun --show-sdk-path)`）。`make -f build.mk run-tests`（-O2）**28/28 通过**（含新增 `formula_fastpaths`）；lib strict `-Wall -Wextra -Werror` 通过。
- CMake 3.31.6 + GCC 16.2 Release `-Wall -Wextra -Werror`：构建通过，`ctest` **33/33 通过**（含示例、PCH、compile-fail、documentation）。
- CMake 3.31.6 + Apple Clang 21 Release `-Wall -Wextra -Werror`：构建通过，`ctest` **33/33 通过**。为此给 REG 宏族生成的辅助常量（`NAME##_address`/`NAME##_window`）加 `[[maybe_unused]]`（0.35.0 起这些常量在部分使用点未引用，Clang `-Wunused-const-variable` 报错；不影响公开语义，常量仍可访问）。
- ASan+UBSan（`-fsanitize=address,undefined`，全套 CTest）：**33/33 通过**，无 sanitizer 报告。
- 新增 `formula_fastpaths`：memo/zone cache 在「finish 后动态挂回调、重入 borrow 拒绝」下的失效语义；不等深 scope 链优先级（0.35.0 compare() 索引 bug 的回归）；dense/strided/Interleaved2/rank-3 ladder（dims(3,2,3) strides(0x800,0x180,0x100)）/非 2 幂 stride(0x14)/空洞/64-bit 宽数组，动态+静态双路对拍。
- 回归：`formula_golden` 1321 word 冻结基线一致；`formula_diff` 20k 随机读写双路一致；`address_view` LayerOrder 三层叠加（same-priority 语义保持：仅对 winner 报错、低优先级 shadow warning×2）通过。
- 清理验证：`grep -R "compiled_views_|compiled_access_|compiled_redirects_|attach_static|StaticLookupSlot" include src` 无匹配；生产 MMIO 分发仅 `FormulaDecoder`。
- MMIO A/B（GCC 16.2 -O2，与冻结 0.34.0 StaticWord 交替测量，ns/access，mixed30）：rw random 33.1（StaticWord 36.2，快 8.5%）；rw mapped 77.5（83.0，快 7%）；rw hot 28.3（24.8，+14%）；callbacks hot 50.6（33.3）；plain random 12.9（0.34 dynamic 26.2，快 52%）。
- 未验证：GCC 13/14、Apple Clang 21 本轮复跑（0.35.0 已过，本轮无平台改动）、真实 QEMU/POE/DAG 业务源码（工作区无源码，待验收）、hlog 插桩 ASan。

# regfw 0.35.0 验证记录（Legacy removal）

- 本机：macOS arm64，Apple clang 21.0.0，C++20。`make -f build.mk -j4 run-tests` **27/27 通过**；`make -f build.mk -j4 lib CXXFLAGS='... -Wall -Wextra -Werror'` 通过；ASan+UBSan（`-fsanitize=address,undefined`, `BUILD=build-asan`）整套 27 项通过，无 sanitizer 报告。
- GCC 16.2.0（Homebrew，macOS arm64，`-isysroot $(xcrun --show-sdk-path)`）：`build.mk` 27/27 通过；CMake Release + `-Wall -Wextra -Werror` 构建通过，`ctest --test-dir build-cmake-gcc` **32/32 通过**。GCC 13/14 仍为历史 CI 数字，未在本机复跑。
- 旧后端删除检查：`grep -R "StaticBuild|StaticWord|StaticShadow|StaticPlanView|compile_static_layout|install_static_layout|REGFW_LEGACY_STATIC_WORDS" include src` 无匹配；`src/static_layout.cpp` 删除；`build.mk`/CMakeLists 不再包含它或旧 benchmark 目标。
- 新增 `formula_geometry`：跨 base 包围盒重叠但绝对字节不冲突的 View 必须接受；reserve `[0x1100,0x3100)` 只遮蔽该区间，区间外寄存器可访问；`ShadowLayout` 验证同优先级 reserve 在运行期遮蔽同字节寄存器且布局可编译；越界/空洞拒绝。旧相对坐标校验会误报，现已修复。
- 新增 `formula_golden`：冻结 0.34.0 旧 builder 的 1321 条 `Battery<1>` word 解码（address/target/binding/linear/byte/available/scope/identity/flow/route/policy），逐条与公式 plan 对齐；`formula golden OK (1321 words)`。
- `formula_diff`：dynamic vs formula 两套布局对 20k 随机读写、identity/access-group、宽寄存器、回调、pre_access、快照交叉恢复、reset 做行为差分；所有 probe 值一致。
- `static_layout`：consteval 静态布局与等价动态布局差分（VF repeat、identity、pre_access、RO alias、reserve、wide、hooks、snapshot）；`symbolic_layout` 静态/动态 warning 计数与访问保持一致。
- 动态布局：`AddressView` 已合并到 `FormulaDecoder`。`ViewCompiler` 为每个映射元素建立 Scalar `Mount`（数组按 `occupied` 共享，binding 携带元素线性索引），`install_dynamic` 生成 zone/page/run 目录；运行期不再构建逐 word `compiled_views_`。`address_view` 的优先级 overlay、slice、8 字节跨实体拒绝和 warning 计数均通过。
- CMake 3.31.6 + AppleClang 21 Release 构建通过；`ctest --test-dir build-cmake` **32/32 通过**（含 27 个测试、2 个示例、`bundle_pch`、`static_compile_fail`、`documentation`）。`cmake/FindHLog.cmake` 现在优先选择 Rust `target/release/libhlog.dylib`，避免 macOS 误链 vendored Linux `.so`。
- 未验证：GCC 13/14（本机为 GCC 16.2）、真实 QEMU/POE/DAG 业务源码（本机仅基准布局）；ASan 的 hlog 未重新插桩。
- 基准：POE 8.68s/623MiB/918KiB；DAG 1.2s/224MiB/96KiB；8 VF/262144 word 0.98s/216MiB/76KiB；DAG 6 init 0.952ms/9MiB。MMIO hot 静态公式 44.3ns / 动态 48.2ns（0.34 dynamic 21.6ns），mapped 113.9/119.4ns（71.7ns），**5% 目标未达成**。

# regfw 0.34.0 验证记录（DS Formula Decoder，历史）

- 本机：macOS arm64，Apple clang 21.0.0，C++20，`-O2`，27/27 本地测试通过（`make -f build.mk run-tests`）。该环境无 GCC/ctest，GCC Release、ASan/UBSan 与完整 CTest 尚待在目标 Linux 复跑，不能把本机 clang 结果当作 GCC 验收。
- `tests/formula_diff.cpp`：编译期对每个旧 `StaticWord` 逐字段比较 binding/linear/byte/target/available/scope/identity/route/policy/flow，并验证旧表空洞仍为未命中；运行期 dynamic/legacy/formula 三模式 MMIO、回调、AccessGroup、identity、pre_access、快照、部分宽事务差分通过。
- 覆盖：32/64/128/512/2048-bit 混合宽度、一维/二维/转置/非单调 `dims(2,3) strides(0x180,0x100)`、稀疏 stride/空洞、包围盒相交但有效字节不冲突的 View、4 实例 VF repeat、PF-shared alias、`(4,8)` 512-bit GET_GROUP 形状、Access Group 重映射、未对齐/越界/错误访问。
- 基准：POE 型 TU 20.1s/1003MiB/3882KiB → 10.4s/658MiB/918KiB；8 VF × 262144 word 104.7s/3010MiB/40574KiB → 0.96s/214MiB/76KiB；DAG 6 实例安装 1.073ms → 0.728ms。Scalar32 hot 42.3ns vs 21.9ns，未达到 5% 门槛，详见 `docs/FormulaBackend.md`。

# regfw 0.33.3 验证记录

- GCC 13.3.0 / C++20 / Release，`-Wall -Wextra -Werror`：31/31 CTest 通过（含文档生成检查）。
- 0.33.3 新增差分覆盖：宽度混合（32/64/128/512/2048-bit 含稀疏 stride）、非单调多维（512-bit dims(2,3) strides(0x180,0x100)，交错地址序）、2048-bit 一维稀疏、互补 View 叠加（包围盒重叠、字节互补）与真实覆盖回退、宽寄存器 identity 别名（512-bit 多入口 + redirect 预算）、`StaticBuild::stats` 断言（mounts/elements/words/redirects/path），全部在 `static_regular` 中与强制完整构建器逐字段对拍。
- `REGFW_REQUIRE_NONOVERLAP_FAST_PATH` 严格模式按 ActualOverlap 分类终止，纳入 static_compile_fail（GCC 13.3/14.2 与 Clang 18 一致）。
- `benchmarks/poe_layout.cpp`：快路径 39.1s/3.39GiB，强制完整 52.7s/4.68GiB；两者均在 268M constexpr ops 上限内完成（此前用户报告的 12GiB 场景为完整构建器 + 更大规模）。
- GCC 14.2.0 / C++20 / Release，`-Wall -Wextra -Werror`：31/31 CTest 通过（独立复核环境）。
- Clang 18 / C++20：`tests/static_compile_fail.py` 全部 12 项编译失败契约（11 项非法布局 + 轻量入口误用）通过；全量测试套件受基线测试源码的 `-Wunused-const-variable` 等 clang 专属告警阻塞（0.32.0 基线同样存在），非本轮引入，框架头文件与公开 API 无 clang 报错。
- RelWithDebInfo / ASan+UBSan / `-fno-omit-frame-pointer`：31/31 通过，无 sanitizer 报错。`ASAN_OPTIONS=detect_leaks=0`；hlog 未重新插桩。
- 新增 static_regular 差分回归：14 组布局（各宽度/偏移、稠密转置、extent=1、稀疏退回、>4 GiB 页目录与 word hash、重复 VF identity 家族 1/4/100 实例、别名双入口、只读入口、pre_access、嵌套实体、重复实体 selector）在常量求值中对直填路径与强制完整路径逐字段对拍（bindings/scopes/identities/words/shadows/redirects/pages/index/addresses 及 source_location）；并对直接填表计划与等价动态布局做 4000 次随机 MMIO 交替对照（identity 重定向、1/4 字节部分访问、快照往返）。
- 发射修复回归：运行期扫描发射后的静态计划索引（真实条目数必须等于 word 数、空洞槽必须保持哨兵值），并对 word-hash 与 page-hash 两种索引做全地址命中探测与 64 组随机未映射地址的有界终止探测。修复前该场景在 0.32.0 基线（GCC 13.3，同一二进制内）表现为查找探测死循环。
- 34 TU 并行构建：`--jobs 16` 与 `--jobs 24` 均通过（含别名/快照运行检查）；`-j32` 在本 7.65 GiB 容器内因内存上限失败（两版皆然），需在 ≥8 GiB 目标环境验收。
- 新增 bundle_light/bundle_pch 回归：仅含 `<regfw/bundle.hpp>` 的消费 TU 声明/句柄/回调可用；`REG_LAYOUT_IMPL` 在完整入口 TU 定义后跨 TU 链接、MMIO、identity、快照通过；`regfw_precompile_bundle` 端到端（PCH 实际被编译器使用）通过。仅含轻量入口时使用 `REG_LAYOUT`/`REG_LAYOUT_IMPL` 由 `static_assert(REGFW_COMPILED_LAYOUT_ENTRY)` 立即拒绝（GCC 13/14、Clang 一致），已纳入 static_compile_fail。
- `benchmarks/plan_digest.cpp` N=64/128/256 完整语义摘要与 0.32.0 逐字节一致（11,168/22,048/43,808 words，哈希相同）。
- 安装后 `find_package(regfw 0.33.3 EXACT)` 三编译单元消费者（静态/动态 DECL/IMPL）沿用 layout_separate 流程构建运行通过。
- 最终版 MMIO 交替对照（未绑定/绑定 INFO × 普通/回调 × 随机/映射 × 读/写）与初始化对照由 `benchmarks/reproduce.py --baseline <0.32.0>` 一键复跑；两版校验和一致。完整数据见 [Performance](docs/Performance.md)。
- 文档生成区域、示例输出和链接检查通过。交付源码不含本轮或历史测量 JSON、日志、二进制中间产物；保留配套 hlog 库、源码、许可证及必要使用说明。


本轮没有真实 QEMU 整机环境，不声称验证整机启动耗时或 QEMU 完整迁移流。框架已有事务/快照和适配接口回归保持。头文件/PCH 成本矩阵（full/light × PCH × 并行）见 Performance。

可复现 sanitizer 构建：

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-asan -j2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure
```

性能复测入口：[benchmarks/README.md](benchmarks/README.md)。布局分离用法：[LayoutCompilation](docs/LayoutCompilation.md)。
