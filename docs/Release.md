# 发布与文档维护

[返回 README](../README.md) · [版本变化](../CHANGELOG.md)

## 更新示例后同步文档

在 SDK 根目录执行（源码树缺少 bundled hlog 时，configure 另传 HLog_ROOT）：

```sh
cmake -S . -B build -DREGFW_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
python3 scripts/update_docs.py --build-dir build
ctest --test-dir build --output-on-failure
python3 scripts/update_docs.py --check --build-dir build
```

文档代码块从 `examples/quickstart.cpp`、`examples/pf_vf_layout.hpp`、`examples/pf_vf.cpp` 的 DOC 标记提取；报告标题来自 `include/regfw/layout_titles.hpp`。`docs/output/` 保存当前 PF/VF 示例生成的输出。不要手工编辑 GENERATED 区域或输出快照。

检查模式只读，任何不同即失败；不带 `--build-dir` 时检查代码片段、标题、版本、公开示例清单和相对 Markdown 文件链接，带该参数还运行示例并逐字比对输出。检查不会用新输出悄悄覆盖旧文档。

## 创建本地发布包

```sh
python3 scripts/release.py --hlog-root /absolute/path/to/hlog --output-dir /tmp/regfw-dist --jobs 2
```

hlog 根目录需完整源码 SDK；发布工具需要 Python ≥ 3.11，重建依赖需要 Cargo。离线依赖已缓存时可传 `--offline`。发布脚本拒绝覆盖已有版本文件；版本来自 CMakeLists.txt。

脚本先生成候选包，再验证实际解包内容：

1. 配套 hlog 的 ABI、Rust 测试、真实 fmt C++17/C++20 位域/hexdump 及 runtime 测试。
2. 默认顶层构建，运行非空且无重名的完整 CTest（包含核心回归和文档检查）。
3. 只开公开示例的独立构建，确认不会编入测试程序。
4. 安装后搬移前缀，构建独立 find_package 消费者。
5. 同一解包源文件的 ASan/UBSan 构建与同一组 CTest。
6. 包内容检查，拒绝日志、构建产物、失效文档链接或额外公开示例。

全部通过才写出 tar.gz、sha256 与 JSON manifest。具体测试名单、验证目录和 sanitizer 设置记录在对应 manifest 中，不在多份教程里手抄测试总数。ASan 使用 detect_leaks=0，当前发布验证环境为 Linux/GCC；真实 QEMU 进程不在此检查范围。
