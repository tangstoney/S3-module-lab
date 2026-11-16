# s3_module_lab

ESP32-S3双串口模块独立调试。业务代码仅`main.c`、`config.h`、`radar.c`、`voice.c`、`shell.c`，不做模块联动。

## 前置检查

实际工程根目录为本README所在的`s3_module_lab/hello_world`。
SDK路径：`D:/esp/v6.1/esp-idf`。直接运行该SDK的`tools/idf.py --version`得到 **ESP-IDF v6.1**，不是v6.0。
PATH中的`idf.py.exe --version`显示`v1.0.3`，那是启动包装器版本，不是SDK版本。

实现前读取了下列6个厂家文件的完整可提取文本、工作表和源代码。资料根目录是`D:/Tangshi/Programs/Hqvt/rd91/外挂模块/`，原文件未修改。

| 子目录 | 实际读取文件 |
|---|---|
| `MS24-2220D58M4-LDO-B-8D-NLS-MCU-FMCW（24G雷达模组）` | `MS24-2220D58M4-LDO-B-8D-NLS-MCU-FMCW产品规格书V1.1_20250212.pdf`，9页 |
| 同上 | `超低功耗模组功能说明.docx` |
| 同上 | `觅感_超低功耗24G_FMCW_AT指令.xls`，`AT指令集`第1～33行 |
| `WT2003Hx（语音芯片）` | `WT2003HM01模块资料 V1.04.pdf`，22页 |
| 同上 | `WT2003Hx系列远程下载使用说明书V2.03.pdf`，32页 |
| 同上 | `WT2003HX远程下载例程（外置flash，内置flash下载的话，把里面的F1创建文件指令换成F6就可以了）.c` |

同时核对本机`uart.h`、`esp_console.h`、console实现、stdio配置和`docs/en/api-reference/peripherals/gpio/esp32s3.inc`。排针/复用参考此前提供的`ATK_DNESP32S3_V1.4.pdf`第1、3页。

## 协议摘要与待确认项

### 雷达MS24-2220D58M4

- 默认115200，模块PB7=RX、PB6=TX；OUT感应输出高约3V、静默0V，默认感应输出时间30秒。
- PB4串口唤醒/使能，低电平有效。最小低电平时间、拉低后等待时间、是否保持至返回完成、释放后行为、内部上拉、RX/PB4输入耐压均**待确认**，代码不自动唤醒或插入猜测延时。
- 厂家资料未明确数据位/校验/停止位，**待确认**。8N1是用户指定的初始配置，可在`config.h`修改。
- 新目录中的AT表明确要求指令和返回均以CRLF结尾。检测开关为`AT+RADAR_DETECT_ONOFF=0/1\r\n`，返回`OK\r\n`或`ERROR\r\n`。开关掉电不保存，关闭后约1uA；关闭后OUT如何变化**待确认**。
- 实现表中的查询：`AT+FW_INFO`、`AT+VER`、`AT+UUID`、`AT+RADAR_DETECT_ONOFF`、`AT+MOVE_RANGE`、`AT+OUT_MODE`、`AT+ON_TIME`、`AT+FMCW_PERIOD`，均追加CRLF，返回`OK:...\r\n`或`ERROR\r\n`。UUID写为16字节，但字符串编码细节**待确认**，只显示返回。
- OUT可被AT配置为反相；状态日志按用户指定的默认正相解释。`radar query out`若返回`OK:1`，有人/无人对应关系将相反，本程序不自动改变模块设置。

### WT2003HM01

- 以HM01 V1.04为封装依据：默认9600、8N1、3.3V TTL，BUSY高表示播放。
- 远程下载V2.03说明默认115200，与HM01不同；实物固件型号/版本及远程下载能力**待确认**，不将两套播放/下载功能混用。
- 普通帧：`7E L CMD PARAM... SUM EF`。`L=参数字节数+3`，整帧长度`L+2`；`SUM=(L+CMD+全部参数字节)&0xFF`，不含头尾和校验自身。
- 本阶段命令：C2状态、C1查询音量、AE设置音量0～31、A0按SPI-Flash根目录索引播放、A6按U盘根目录索引播放、AB停止、C3查询Flash音频总数、C9查询当前曲目索引、CA查询外设。
- 播放索引为2字节，大端。shell接受0～65535原始索引，手册第一首示例是`00 01`；0是否有效**待确认**，实际文件范围以模块返回为准。
- 写命令应答示例`7E 04 A0 00 A4 EF`：参数00成功、01命令出错、02无文件、05设备不在线；HM01未定义的返回码标为“待确认”。
- 状态查询发`7E 03 C2 C5 EF`，返回示例`7E 04 C2 02 C8 EF`，状态01播放、02停止、03暂停。
- 音量返回`7E 04 C1 VV SUM EF`；C3/C9返回两个参数，按示例大端解读。CA中BIT3=PC、BIT2=U盘、BIT0=SPI-Flash，其他位按HM01标“待确认”。
- 已读取F1/F6、FD、F0、F7下载示例，第一阶段不实现下载、格式化、删除。两版本A1文件名限制不同；HM01的CB描述字节数与示例不一致；V2.03的C4返回命令字示例与标题不一致，均**待确认**，不封装这些功能。
- V2.03建议指令间隔200～300ms。本项目采用250ms发码间隔策略，包含原始透传；过快直接返回错误。它是否属于实物HM01的强制要求**待确认**。

## 接线和GPIO检查

一行对应一根线。模块列的TX/RX按模块自身定义，S3列注明发送/接收用途。两个模块与开发板必须共地。

**雷达模块**

| 雷达模块引脚 | 接到开发板 |
|---|---|
| 第8脚 VIN/VCC | 5V电源 |
| 第7脚 GND | GND |
| 第4脚 PB7/RX（模块接收） | GPIO4（S3发送，P1第1脚） |
| 第5脚 PB6/TX（模块发送） | GPIO5（S3接收，P1第2脚） |
| 第6脚 OUT | GPIO6（输入，P1第3脚） |
| 第3脚 PB4 | GPIO7（开漏控制，P1第4脚） |

**语音WT2003HM01模块**

| 语音模块引脚 | 接到哪里 |
|---|---|
| 第2脚 VBAT | 开发板5V电源 |
| 第1脚 GND | 开发板GND |
| 第4脚 RX（模块接收） | GPIO15（S3发送，P1第5脚） |
| 第3脚 TX（模块发送） | GPIO16（S3接收，P1第6脚） |
| 第5脚 BUSY | GPIO17（输入，P1第7脚） |
| 第7脚 SPK+ | 外接喇叭一端 |
| 第8脚 SPK- | 外接喇叭另一端，不接GND |
| 第16脚 VDDIO | 本次留空，不外接电源 |

**电脑连接**：电脑USB数据线接开发板标有UART的Type-C接口，通过板载CH340连接UART0（GPIO43发送、GPIO44接收），终端波特率115200。

P4按原理图连接1-3、2-4，VOUT2是5V/GND排针。语音VDDIO是输出，不与S3的3V3并接。非HM01板型需要重新核对模块脚号。雷达RX/PB4的3.3V兼容性需厂家确认，不可因5V供电就假定5V逻辑。

本机ESP32-S3文档：strapping=GPIO0/3/45/46；通常flash/PSRAM占26～32，八线flash/PSRAM还占33～37。所用4/5/6/7/15/16/17不与其相交，未改用户引脚。它们与开发板摄像头/RGB屏共用，请断开这些外设。原生USB的19/20、UART0的43/44未被模块占用。未读取实物flash/PSRAM容量，本阶段不启用PSRAM，沿用SDK默认flash容量设置。

## 构建与烧录

本机constraints文件位置与常规export布局不同，中文构建路径会导致GCC的specs响应文件解析失败，ccache处理中文源路径也会抛出异常。`idf-local.ps1`使用英文路径的工具映射和构建目录，并关闭ccache，引用原安装，不修改SDK；直接调用SDK的Python脚本，避免启动包装器版本混淆。

在本目录PowerShell执行：

```powershell
.\idf-local.ps1 Version
.\idf-local.ps1 SetTarget
.\idf-local.ps1 Build
```

默认构建目录：`%USERPROFILE%/.cache/codex-s3-module-lab/build`。完整英文构建路径是必要的，仅盘符映射会被idf.py解析回原路径。

VS Code ESP-IDF扩展2.3.0的项目配置也已对齐该构建目录：`idf.buildPath`使用同一路径，`idf.enableCCache=false`，CMake显式关闭ccache，环境设`PYTHONUTF8=1`、`IDF_CCACHE_ENABLE=0`，clangd也读取该目录。修改后执行`Developer: Reload Window`再使用扩展的Build/Flash/Monitor按钮。无需重新自动扫描串口，项目端口已设COM6。

硬件接线确认后自行刷写，COM6按实际修改：

```powershell
.\idf-local.ps1 Flash -Port COM6
.\idf-local.ps1 Monitor -Port COM6
```

## 首次调试

电脑终端115200。每次输入一条，观察异步返回后再输下一条；语音间隔至少250ms，不要将整段一起粘贴。

```text
help
status
radar watch off
radar wake on
radar query version
radar query fw
radar query uuid
radar query detect
radar query out
radar query range
radar query on_time
radar query period
radar detect off
radar query detect
radar detect on
radar watch on
voice status
voice volume
voice query devices
voice query count
voice volume 10
voice play flash 1
voice query track
voice stop
```

`radar wake on`只拉低PB4，完整唤醒时序**待确认**。结束调试可`radar wake off`释放，高阻不保证PB4实际为高，需要模块自身或合适的外部上拉。

原始透传和ESP端波特率示例：

```text
uart text radar "AT+VER" --eol crlf
uart hex voice 7E 03 C2 C5 EF
uart baud voice 115200
```

最后一条只修改ESP端，**模块端未修改**。无返回时核对供电、共地、交叉TX/RX、波特率、PB4和语音模块USB数据线是否已拔掉。

## 并发和边界

- 参数集中在`main/config.h`。UART0由sdkconfig初始化，编译期检查UART号/波特率一致。
- UART1/2各由自己的接收任务独占读取，另有OUT边沿日志任务和esp_console任务。OUT使用GPIO6双边沿中断，中断只投递电平，日志在任务中打印。启动或`radar watch on`时显示一次当前状态，此后只在有人/无人变化时打印，不再每秒刷屏；`radar watch off`关闭这些日志。待处理事件合并为最新电平，极快翻转可能合并，本功能用于状态监视而非脉冲计数。每模块一个发送锁，等待最多200ms。
- 无TX环形缓存，用非阻塞`uart_tx_chars`及3000ms总截止时间发送/等待FIFO完成，失败显示已入FIFO字节数，可能已有残帧。语音间隔检查也在同一锁内。
- 原始输出按32字节片段打印HEX/TXT，不可打印ASCII以`.`替换；片段不是协议帧。持续大量输出可能受UART0日志带宽限制，溢出会报告并重同步，不保证高吞吐无损转发。
- 语音按头、长度、校验、尾解析，保留跨读取残帧，支持粘帧、噪声和负载内7E/EF；错误时滑动重同步。500ms字节间残帧超时是本机策略，非厂家时序；超时仍尝试恢复残帧里隐藏的完整帧。
- HEX非空，每字节恰好两位，最多256字节，无自动换行。text默认none，仅允许none/cr/lf/crlf，总发送长度含结尾最多256字节。字面`\r\n`不自动解码，使用`--eol crlf`。
- 波特率1200～921600是应用限制，不表示厂家支持全部值。OUT/BUSY未连接时浮空读数不能代表模块状态。
- 发送成功不等于模块执行成功，以返回为准；异步输出可能打断提示，可先`radar watch off`。不实现自动重试、事务队列、持久化、联动。

## 验证

协议测试直接提取生产`voice.c`的编码器/解析器，RISC-V GCC编译后由Unicorn执行真实机器码。只替换硬件输出/打印，不重写协议算法。

```powershell
python -m pip install --target tests/.deps -r tests/requirements.txt
python tests/test_voice_host.py
```

11项测试覆盖手册帧编码、非法编码参数、全部拆分位置、粘帧/噪声、坏校验、坏尾、非法长度、负载内头尾、错误长度后超时恢复、过期残帧和257字节最大帧。它们不验证S3外设或FreeRTOS运行。

实机待验收：UART0 shell、PB4开漏低电平/释放、AT查询和检测开关、OUT及保持时间、语音查询/音量/播放/停止/BUSY、并发、错误参数、发码过快、串口错误后恢复。可用逻辑分析仪核对波特率、CRLF及HEX实际字节。未刷写或读取实机，不将构建和协议测试记为硬件通过。
