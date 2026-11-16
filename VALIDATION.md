# 验证记录

## 2026-11-16：OUT改为状态变化日志

- GPIO6使用双边沿中断，ISR只投递最新电平，由任务打印；移除每秒重复打印任务。
- 启动或`radar watch on`显示一次当前状态，之后仅状态变化打印；`radar watch off`仍可关闭日志。待处理边沿合并到最新电平，不用于高速脉冲计数。
- 执行`idf-local.ps1 Build`，退出码0，生成镜像并通过分区尺寸检查；应用大小`0x38230`字节（约224.5 KiB），1 MiB应用分区剩余78%。
- 固件SHA-256：`97a55a17cabe369cc53def75a78a84f6f6157575122b69063bffad4d491bcdfd`。
- 此次修改未刷写实机；上升沿、下降沿和shell输入体验待用户刷写后确认。

## 2026-11-13：初始版本及构建路径修复

VS Code构建路径修复：已按本机ESP-IDF扩展2.3.0配置schema设置`idf.buildPath`指向成功构建目录，关闭ccache并启用Python UTF-8，clangd路径同步。读取项目设置中的CMake参数实际执行配置及`ninja all`，均退出0，重新生成固件且尺寸检查通过。未操作VS Code界面或刷写硬件，扩展需Reload Window加载新配置。

## 本机环境

- 实际SDK：`D:/esp/v6.1/esp-idf`，`tools/idf.py --version`输出`ESP-IDF v6.1`。
- Python：`D:/Espressif/tools/python/v6.1/venv/Scripts/python.exe`，3.11.15；IDF Python依赖检查通过，使用安装中已有的v6.1 constraints文件。
- 目标：`esp32s3`。本机C头文件和console实现为API依据。
- 构建目录：`C:/Users/tangshi/.cache/codex-s3-module-lab/build`。

## 构建结果

执行SDK的`idf.py -C <工程目录> -B <上述构建目录> set-target esp32s3`，退出码0。
执行`idf.py -C <工程目录> -B <上述构建目录> --no-ccache build`，退出码0。

```text
Successfully created ESP32-S3 image.
Generated C:/Users/tangshi/.cache/codex-s3-module-lab/build/s3_module_lab.bin
s3_module_lab.bin binary size 0x37b80 bytes.
Smallest app partition is 0x100000 bytes.
0xc8480 bytes (78%) free.
Project build complete.
```

应用228224字节（222.875 KiB）。bootloader大小0x5280字节。
固件SHA-256：`1d90356f10f277783b5f288e73fe353d31a74f1ad97346b9f268f89541b670c9`。

原工程已有未完成的build配置，已保留为`build.before-module-lab`。初次构建先后发现：工具安装布局与export默认不符、中文路径specs解析失败、ccache无法转换中文源路径。通过独立工具映射、英文构建目录及`--no-ccache`解决，没有修改SDK文件。SDK自身Kconfig有无0x前缀的弃用提示，不影响构建。

## 语音协议自动测试

命令：`python tests/test_voice_host.py`。11项全部通过，实际运行生产编码器/解析器的RISC-V机器码，不使用Python重写协议。

1. 手册示例帧编码：C2、C1、AB、AE、A0。
2. 拒绝过长参数和空指针参数。
3. 返回帧每个可能的拆分位置。
4. 粘帧和噪声。
5. 坏校验后恢复。
6. 坏帧尾后恢复。
7. 非法长度0、1、2后恢复。
8. 参数内包含7E和EF。
9. 错误大长度隐藏有效帧，超时后恢复。
10. 过期残帧后接收新帧。
11. 最大257字节返回帧，逐字节输入。

## 检查范围

- 编译/链接/镜像与分区尺寸：通过。
- 协议编码、拆帧/粘帧/错误恢复：上述自动测试通过。
- GPIO与本机文档列出的flash/PSRAM/strapping脚：静态核对无交集。
- 发送超时、互斥、参数校验、接收任务独占关系：代码复查，尚无S3运行验证。
- 未执行刷写、串口读写或实机测试。PB4电平、AT应答、OUT保持时间、语音BUSY、播放效果、实际固件版本、双模块并发及串口错误恢复均待实机验证。

随工程提供的 `idf-local.ps1 Version` 和 `idf-local.ps1 Build` 均已执行，退出码0。删除失败构建目录的清理操作被自动审批以“blocked by policy”拦截，相关目录保留，不影响英文路径下的成功构建。
