# regfw 0.36.0 — 正式源码交付与验证报告

## 本次目标和结果

本次以 0.35.2 Prepared Access Hash 原型为基线，将默认静态布局从两遍 `consteval` FormulaPlan 构建切换到普通 C++ 单遍布局描述生成，并保留已验证的共享 EntityLayout Hash + PreparedAccessInfo 访问分发。

**用户面 API 不变**：`REG`、`REG_BUNDLE`、`REG_LAYOUT`/`REG_LAYOUT_IMPL`、`REG_LAYOUT_DECL`、View、Repeat、Identity、Resolver、Access Group、PreAccess、Callback、Transaction、Snapshot、MMIO 接口均沿用原写法；布局声明/实现分离 TU 和 CMake 安装包继续支持。

**从生产实现移除**：
- 两遍完整 `consteval` 的 `compile_formula_layout()` 及定容 `FormulaPlan<B,S,I,M,A>`。
- 编译期非单调寄存器数组排序；现改为原生运行时一次性按声明类型构建的只读描述。
- 旧的 Zone、Candidate、ScalarRun、PageDirectory 地址分发分支、实验切换宏及热路径可变缓存。
- 不再使用过期的编译期运算限额作为默认构建参数。

**保留**：动态 AddressView、共享 REG/Entity 描述、所有运行期存储/执行语义；手工 RegisterMap API 使用的底层通用查找工具不可当作废弃代码删除。

## 实际验证环境

- Linux x86-64，GCC 14.2，C++20，Release `-Wall -Wextra -Werror`。
- 完整 CMake/Ninja 构建成功；CTest **35/35 通过**。
- Clang 17，Release `-Wall -Wextra -Werror` 核心库独立构建成功。
- `cmake --install` 成功；外部项目 `find_package(regfw 0.36.0 EXACT REQUIRED CONFIG)` 成功，并实际完成 MMIO 读写。
- 原有编译期类型/声明错误测试继续保留；需要实际布局几何才能判断的非法布局通过新 `static_runtime_errors` 运行时验证测试覆盖。

## 编译期对照（合成 POE 形状，不是真实用户布局）

对同一个 `benchmarks/poe_layout.cpp`，GCC 14.2、`-std=c++20 -O2 -DNDEBUG -fno-strict-aliasing -fsyntax-only`，未人为提高 constexpr ops limit：

| 对象 | 结果 | 墙钟耗时 | 编译峰值 RSS |
|---|---|---:|---:|
| 0.35.2 原版 | **失败**（GCC 默认 constexpr ops limit 33,554,432） | 29.62 秒 | 1,891,204 KiB |
| 0.36.0 新版 | **成功** | 15.30 秒 | 1,001,836 KiB |

该对照表明新版无需旧的完整 `consteval` 过程就能够编译这一代表性布局，但由于旧版未成功，**不能把两行当作严格的成功编译耗时加速比**；原用户真实 POE 布局曾需约四分钟，本次环境不具备该完整源码，不能宣称已测出它的实际耗时。


## 合成大数组初始化测量（单次；非真实 POE）

使用可选基准 `bench_scale_formula`，8 个 VF 窗口、512-bit 二维寄存器数组；GCC 14.2 Release：

- `construct=0.013770s`，`finish=0.000022s`，进程峰值 RSS **8,424 KiB**。
- Scalar32 写约 **20.0 M ops/s**、读约 **27.5 M ops/s**，identity 映射写约 **7.13 M ops/s**（仅此合成基准）。

单独的 `bench_init_formula dag 1` 输出构造 CPU 约 **3.32 ms**、`finish` CPU 约 **0.089 ms**，进程峰值约 **5,516 KiB**。这两个基准证明所测形状下运行时建表没有造成数秒级停顿；不能用它推算你真实 POE/DAG 的 MMIO TPS、machine init 或 10 模块累计时间。

## 性能与正确性边界

- `REG_LAYOUT_IMPL` 对应的布局工厂首次使用时由普通 C++ 执行并生成不可变描述。Layout 的几何合法性检查从 **编译期** 转到 **初始化期**；发现不合法几何时仍须拒绝安装。
- View 挂载复用共享描述；各 Entity 实例的 Storage、Resolver、Callback 等动态状态不会错误共享。
- 有效地址 Hash 的规模仍取决于实际可访问 **4-byte lane** 数量。大量宽 REG 可能增加首次安装的时间和堆内存；**不能把编译期节省等同于 machine init 免费**。
- 当前环境未提供用户真实 QEMU POE/DAG 工程，尚未验证：真实布局的四分钟编译是否降至何值、十个模块的累计初始化耗时、整机快照迁移和 MTTCG 并发约束。
- 本次以消除废弃编译期建表为主；现有微基准不能证明 MMIO 延迟已优于用户原手写 switch-case。

## 可复现构建

```sh
sha256sum -c SHA256SUMS
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Werror'
cmake --build build -j2
ctest --test-dir build --output-on-failure
cmake --install build --prefix /tmp/regfw-0.36.0-install
```

自带 HLog 运行库在 `third_party/hlog`。独立消费工程可设置 `-DCMAKE_PREFIX_PATH=/tmp/regfw-0.36.0-install -DHLog_ROOT=/path/to/regfw-0.36.0/third_party/hlog`。

## 源码规模

本次 `include/` + `src/` 的 35 个 C/C++ 文件共 **9,960 物理行**，基线为 **10,377 行**，净减少 417 行。无删减有效 CTest。只把减少代码行数当作辅助信息，不作为正确性或性能论证。
