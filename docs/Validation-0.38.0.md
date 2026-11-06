# 0.38.0 验证说明

此版本新增保持即时寄存器语义的可选异步端点，并修复同一 executor 并发 drain 的活锁。接口见 [Async](Async.md)，设备迁移与适用边界见 [设备改造指南](Async-Device-Guide.md)。原直接 Map API 保留，头文件和编译库必须配套更新。

## SDK 验证与复现

在 Linux x86_64 / GCC 13.3 环境下，使用 clang-format 17.0.6，并按 GitHub CI 的 `-Wall -Wextra -Werror` 配置检查。完整 CTest 为 36 项；异步回归还单独使用 ThreadSanitizer。安装后的独立 CMake 消费者验证 find_package(regfw)、Threads 依赖及公开异步 API。

```sh
bash scripts/restore_hlog.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Werror'
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
bash scripts/format_cpp.sh --check
```

源码仓库的 `SHA256SUMS` 覆盖受版本控制的普通文件（不含清单自身、符号链接及解压恢复的依赖产物），可在仓库根目录执行 `sha256sum -c SHA256SUMS` 验证。

线程竞争检测：

```sh
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer -fno-pie' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread -no-pie'
cmake --build build-tsan --target regfw_async_test --parallel 2
build-tsan/regfw_async_test
```

验证宿主的 WSL2 地址布局与 GCC TSan 存在冲突，因此实际 TSan 运行使用 `setarch x86_64 -R build-tsan/regfw_async_test`，只调整该进程的 ASLR，没有修改系统设置。具体宿主是否需要此选项取决于 TSan/内核环境。

异步回归覆盖即时 RW、W1C/RC/FIFO、半字/宽事务、命令参数捕获、PF/VF 动态路由、队列满时变更前拒绝、前后台原始位域竞争、四端点互访和关闭后继任务。并发 drain 回归包括 2/8/80 个调用者、容量 1 的满队列、多代后继、正在执行和失败的任务、预留提交/取消、worker 自等拒绝与空闲返回。

## 独立 QEMU 实验环境

QEMU 10.1.0、Linux 6.12.57、ARM64 virt/Cortex-A72、8 vCPU、MTTCG。实验环境维护在本次开发的父工程中，不属于这个 SDK 仓库；以下是已保存的实验结果，单独克隆 SDK 无法运行父工程的脚本。

- 修复后 5,000 轮/核：38 项通过（16 同步回归、15 即时状态/DMA/timer、7 SDK 专项）。A/B/C/D 共用一个 executor，四设备错误、拒绝和 BQL 泄露均为 0，正常退出。
- 功能开发阶段完成 100,000 轮/核：800,000 条 ADCDBCA 链 / 5,600,000 次寄存器访问；同步、异步各 800,000 次 NEON 两字节写后单次读回；设备 CPU work 和驱动 MSR 各 8,000 次并通过 MRS 校验。
- 长压测早于 drain 修复；修复后重新运行了上述 5,000 轮/核集成验证，未将旧性能数据描述成新版本重新测量。发布整理中的格式调整和显式空成员初始化另行通过严格 SDK 构建验证。

这些结果支持已覆盖的用例，不保证任意设备模型都可无修改异步化。IRQ、任意 guest reset/flush、热拔插/迁移、多 executor 的 QEMU 集成和真实负载的锁尾延迟尚未完整验收。此处不宣称运行了包含 hlog Rust 重编译、全套 ASan/UBSan 的完整二进制打包流水线。
