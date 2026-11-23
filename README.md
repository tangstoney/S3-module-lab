# s3_module_lab

ESP32-S3双串口模块调试 + 雷达/语音报警 + NimBLE BLE主机。业务代码在`main/`：`main.c`、`config.h`、`radar.c`、`voice.c`、`voice_dl.c`、`shell.c`、`alarm.c`、`key.c`、`ble_central.c`、`fob_host.c`、`fob_proto.c`。雷达、按键、语音报警和S3-FOB遥控器已联动；s3-smoke业务协议尚未定义。

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

- 以HM01 V1.04为封装依据：手册默认9600、8N1、3.3V TTL，BUSY高表示播放。**厂家已确认本实物固件为115200**，本项目按115200使用。
- 远程下载V2.03也说明默认115200；本实物播放/下载均按厂家确认的115200，不再套用HM01手册的9600。其他板型仍以实测为准。
- 普通帧：`7E L CMD PARAM... SUM EF`。`L=参数字节数+3`，整帧长度`L+2`；`SUM=(L+CMD+全部参数字节)&0xFF`，不含头尾和校验自身。
- 本阶段命令：C2状态、C1查询音量、AE设置音量0～31、A0按SPI-Flash根目录索引播放、A6按U盘根目录索引播放、AB停止、AC下一曲、AF单曲循环/不循环、C3查询Flash音频总数、C9查询当前曲目索引、CA查询外设。
- 远程下载：按V2.03实现F1/F6创建文件、FD文件大小、F0每包512字节。默认先试片外Flash(F1)，失败再试片内Flash(F6)。下载期间ESP端拒绝其他语音发码，失败发送F7退出下载状态。该功能由`CONFIG_LAB_VOICE_DOWNLOAD_AUDIO`控制，默认关闭，只有开启时才把`main/audio/0001.wav`（约94 KiB）编进固件。
- 播放索引为2字节，大端。shell接受0～65535原始索引，手册第一首示例是`00 01`；0是否有效**待确认**，实际文件范围以模块返回为准。
- 写命令应答示例`7E 04 A0 00 A4 EF`：参数00成功、01命令出错、02无文件、05设备不在线；HM01未定义的返回码标为“待确认”。
- 状态查询发`7E 03 C2 C5 EF`，返回示例`7E 04 C2 02 C8 EF`，状态01播放、02停止、03暂停。
- 音量返回`7E 04 C1 VV SUM EF`；C3/C9返回两个参数，按示例大端解读。CA中BIT3=PC、BIT2=U盘、BIT0=SPI-Flash，其他位按HM01标“待确认”。
- 已读取F1/F6、FD、F0、F7下载示例；本工程按V2.03实现了F1/F6创建文件、FD文件大小、F0每包512字节传输，失败发F7退出，详见“语音文件下载”。格式化、删除、A1/C4等仍未封装。两版本A1文件名限制不同；HM01的CB描述字节数与示例不一致；V2.03的C4返回命令字示例与标题不一致，均**待确认**。
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

VS Code ESP-IDF扩展2.3.0的项目配置也已对齐该构建目录：`idf.buildPath`使用同一路径，`idf.enableCCache=false`，CMake显式关闭ccache，环境设`PYTHONUTF8=1`、`IDF_CCACHE_ENABLE=0`，clangd也读取该目录。修改后执行`Developer: Reload Window`再使用扩展的Build/Flash/Monitor按钮。项目端口以`.vscode/settings.json`的`idf.port`为准（当前COM7）；`idf-local.ps1`默认COM6，刷写前按实际串口改。

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
voice next
voice loop on
voice loop off
key
alarm on
alarm off
alarm
ble status
ble scan on
ble scan off
ble pair
voice volume
voice query devices
voice query count
voice volume 10
voice play flash 8
voice query track
voice stop
```

当前模块的报警音频是`flash 8`；KEY2按下、以及布防状态下雷达有人时的循环报警，都会播放这条音频（后者先发`AF 01`启用模块单曲循环）。联调时也可以先用`voice play flash 8`手动确认。

## 报警联调（临时）

- 雷达OUT=高按“有人”处理（厂家默认正相；AT+OUT_MODE改为反相后本程序不自动适配）。
- 上电默认布防（`LAB_ALARM_ARMED_ON_BOOT`）。布防或PANIC且报警条件成立（雷达有人/烟感置位/遥控器PANIC）时，报警任务先发`AF 01`设为单曲循环，再发`A0 00 08`播放一次，模块自己循环；条件消失或撤防时发`AB`停止。
- KEY0（XL9555 P1_7）切换布防/撤防，撤防会停止并静音当前报警（包括KEY2手动播放的一次和遥控器PANIC）；KEY2（P1_5）手动播放一次报警音；KEY3（P1_4）长按3秒切换BLE配对模式。
- 也可用shell联调：`alarm on`布防、`alarm off`撤防、`alarm`查询当前状态、`voice loop on|off`手动设置单曲循环、`status`查看雷达有人状态。
- 后续由另一块板子控制时，可直接复用`alarm_set_armed()`和`alarm_armed()`。报警索引在`config.h`的`LAB_ALARM_FLASH_INDEX`，循环命令为`LAB_VOICE_CMD_LOOP`。

`radar wake on`只拉低PB4，完整唤醒时序**待确认**。结束调试可`radar wake off`释放，高阻不保证PB4实际为高，需要模块自身或合适的外部上拉。

原始透传和ESP端波特率示例：

```text
uart text radar "AT+VER" --eol crlf
uart hex voice 7E 03 C2 C5 EF
uart baud voice 115200
```

最后一条只修改ESP端，**模块端未修改**。无返回时核对供电、共地、交叉TX/RX、波特率、PB4和语音模块USB数据线是否已拔掉。

## 语音文件下载（一次性）

`main/audio/0001.wav`是16 kHz、16 bit、单声道PCM，约94 KiB。默认**不**编进固件；只有需要给WT2003Hx写音频时才打开：

1. 在`sdkconfig`中设置`CONFIG_LAB_VOICE_DOWNLOAD_AUDIO=y`（或`idf.py menuconfig`进入`s3_module_lab`菜单打开）。
2. 编译烧录。此时应用增加约96 KiB。
3. 终端执行`voice download`；也可用`voice download f1`（片外Flash）或`voice download f6`（片内Flash）强制指定。
4. 等待`[voice] 下载完成`。期间不要发送其他语音命令；失败会发送F7退出下载状态。
5. 下载完成后把`CONFIG_LAB_VOICE_DOWNLOAD_AUDIO`改回`n`再编译，之后的固件不再包含音频。

协议顺序为：AB停止→F1/F6按文件名创建→FD总字节数→F0分512字节传输；模块每包回`7E 04 F0 00 F4 EF`，全部完成回显FD帧表示升级成功。下载使用115200、8N1，模块USB数据线必须拔掉。

## BLE主机（S3-FOB / s3-smoke）

本板是NimBLE Central。`S3-FOB`遥控器已按`docs/ble-host-integration.md`实现，主机端已接入配对、加密、订阅和命令回执；`s3-smoke`协议仍未定义，只做通用发现/订阅/日志。

### 配对（KEY3长按）

- 长按本板KEY3（XL9555 P1_4）3秒，或执行`ble pair`，切换主机配对模式；窗口120秒。
- 在窗口内长按遥控器PAIR键3秒，遥控器广播`S3-FOB-PAIR` 60秒。
- 主机**只在配对模式**扫描`S3-FOB-PAIR`；平时只扫描`S3-FOB`且必须已有绑定，不会看到配对广播就自动绑定。
- 配对使用LE Secure Connections + Just Works + bonding；bond存NVS（`CONFIG_BT_NIMBLE_NVS_PERSIST=y`，`CONFIG_BT_NIMBLE_MAX_BONDS=4`）。
- 遥控器已绑定时长按PAIR不会重新进入配对，需先长按遥控器PAIR **10秒解绑**，再按本板KEY3重新配对。
- 配对成功日志`S3-FOB配对成功`；主机订阅EVENT并向遥控器写STATE。

### 正常命令链路

- 遥控器按键时广播`S3-FOB`约6秒；主机持续扫描，连接后先加密，再发现服务、订阅EVENT。
- 服务`6f0b0001-5e3a-4f7c-9a1d-0b0f0b0f0b01`；EVENT `6f0b0002-...` Notify；HOST `6f0b0003-...` Write（要求加密）。
- 命令帧7字节：`A5 01 01 opcode seq_lo seq_hi crc8`；opcode 1=ARM、2=DISARM、3=PANIC、4=CLEAR_ALARM。
- 主机500ms内写8字节ACK：`5A 01 02 opcode seq_lo seq_hi result crc8`，随后写7字节STATE：`5A 01 03 protection battery flags crc8`。
- CRC8 poly `0x07`、init `0x00`；按每把遥控器用`opcode+seq`去重，重复命令只重发ACK、不重复执行；`last_seq`存NVS的`fob`命名空间。
- protection：1=撤防、2=布防、3=报警；battery固定`0xFF`。
- PANIC置报警标志，撤防状态也会响；CLEAR_ALARM只清除PANIC报警，雷达/烟感报警请用DISARM或KEY0。
- 手机作为central连本板的peripheral角色**尚未实现**；当前只做对遥控器的central。

### 构建前置（`MINIMAL_BUILD`需要注意）

- `main/CMakeLists.txt`的`PRIV_REQUIRES`已加`bt nvs_flash nimble_central_utils`；`main/idf_component.yml`用`${IDF_PATH}/examples/bluetooth/nimble/common/nimble_central_utils`路径依赖。
- `sdkconfig`/`sdkconfig.defaults`已加NimBLE Central、SMP SC、NVS bond、2个连接、3个controller活动、关闭SW coex、日志WARNING。`bt`组件公开依赖`esp_wifi`，所以Wi-Fi组件仍在构建图中，但本程序不初始化Wi-Fi。
- Bluetooth菜单只有在`bt`进入构建后才存在。如果构建报`CONFIG_BT_ENABLED`未定义或NimBLE头文件找不到，先确认`main/CMakeLists.txt`没有被改回，再删除`sdkconfig`/`dependencies.lock`重新生成；`sdkconfig`是本地文件，已gitignore。
- BLE会额外占约250–350 KB Flash和35–60 KB内部RAM；当前1 MB factory分区加96 KB音频仍可能够，若链接报app partition overflow，改`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`（1.5 MB）并整片擦除。
- 当前`main/CMakeLists.txt`把`bt`、`nvs_flash`、`nimble_central_utils`作为无条件依赖，`CONFIG_BT_ENABLED`必须保持`y`；关闭蓝牙会在编译/链接阶段失败。

命令：

```text
ble status
ble pair
ble scan on
ble scan off
ble connect fob
ble connect smoke
ble disconnect all
ble log smoke off
```

`ble status`显示每台设备的状态、地址、conn handle、MTU、S3-FOB的EVENT/HOST句柄、发现/订阅计数、通知计数，以及配对模式和FOB绑定状态。

设备名和扫描方式在`idf.py menuconfig`的`s3_module_lab`菜单：

- `LAB_BLE_FOB_NAME`默认`S3-FOB`（普通命令）
- `LAB_BLE_FOB_PAIR_NAME`默认`S3-FOB-PAIR`（配对窗口）
- `LAB_BLE_SMOKE_NAME`默认`s3-smoke`
- `LAB_BLE_SCAN_ACTIVE`默认`y`（遥控器名字在扫描响应里，必须保持主动扫描）

s3-smoke仍未定义UUID/载荷，`LAB_BLE_SMOKE_HOOK`默认关闭，只是台架占位；不要在产品构建打开。

当前限制：名称匹配依赖外设把名字放进广播/扫描响应；遥控器使用公共地址。遥控器空闲不广播，主机必须持续扫描。遥控器已知的2秒保链定时器可能在配对刚完成时断开；若`S3-FOB-PAIR`仍在广播，主机会在配对窗口内重连重试。手机peripheral服务、多把遥控器同时在线尚未实现。

## 并发和边界

- 参数集中在`main/config.h`。UART0由sdkconfig初始化，编译期检查UART号/波特率一致。
- UART1/2各由自己的接收任务独占读取，另有OUT边沿日志任务和esp_console任务。OUT使用GPIO6双边沿中断，中断只投递电平，日志在任务中打印。启动或`radar watch on`时显示一次当前状态，此后只在有人/无人变化时打印，不再每秒刷屏；`radar watch off`关闭这些日志。待处理事件合并为最新电平，极快翻转可能合并，本功能用于状态监视而非脉冲计数。每模块一个发送锁，等待最多200ms。
- 无TX环形缓存，用非阻塞`uart_tx_chars`及3000ms总截止时间发送/等待FIFO完成，失败显示已入FIFO字节数，可能已有残帧。语音间隔检查也在同一锁内。
- 原始输出按32字节片段打印HEX/TXT，不可打印ASCII以`.`替换；片段不是协议帧。持续大量输出可能受UART0日志带宽限制，溢出会报告并重同步，不保证高吞吐无损转发。
- 语音按头、长度、校验、尾解析，保留跨读取残帧，支持粘帧、噪声和负载内7E/EF；错误时滑动重同步。500ms字节间残帧超时是本机策略，非厂家时序；超时仍尝试恢复残帧里隐藏的完整帧。
- HEX非空，每字节恰好两位，最多256字节，无自动换行。text默认none，仅允许none/cr/lf/crlf，总发送长度含结尾最多256字节。字面`\r\n`不自动解码，使用`--eol crlf`。
- 波特率1200～921600是应用限制，不表示厂家支持全部值。OUT/BUSY未连接时浮空读数不能代表模块状态。
- KEY0/KEY2/KEY3不在ESP32 GPIO上：板载KEY0～KEY3接XL9555扩展IO的P1_7～P1_4，KEY0=P1_7、KEY2=P1_5、KEY3=P1_4，按下为低。I2C使用GPIO41/42、地址0x20；30 ms去抖，KEY3长按3秒切换BLE配对。
- **临时约定（等自定义板到货后修改）**：当前模块的报警音频在Flash索引8。KEY2手动播放；KEY0切换布防/撤防；布防且报警条件成立时循环播放。索引和命令集中在`config.h`的`LAB_ALARM_FLASH_INDEX`、`LAB_VOICE_CMD_PLAY`。
- 语音文件下载期间`lab_send`拒绝其他语音发送并保持互斥；下载是同步命令，控制台在此期间不处理新命令。
- 报警任务每100ms读一次雷达OUT；布防或PANIC且报警条件成立时先发`AF 01`单曲循环，再发`A0 00 08`启动，条件消失或撤防时发`AB`停止。它与KEY2、语音下载共用语音发送锁和250ms发码间隔策略；AF与A0之间等待300ms。
- NimBLE主机任务优先级21、固定在core 0；GAP/GATT回调只做短日志和状态更新，通知通过队列交给`ble_evt`任务，S3-FOB命令解析/ACK/STATE也在该任务执行，不在NimBLE回调里调用`voice_command`/`lab_send`/`vTaskDelay`。BLE初始化失败不阻止雷达/语音/按键，`ble status`显示未初始化。
- BLE与UART/I2C/GPIO无引脚冲突；不初始化Wi-Fi，已关闭`CONFIG_ESP_COEX_SW_COEXIST_ENABLE`减少共存代码（Wi-Fi组件仍随`bt`的公开依赖进入构建）。
- 发送成功不等于模块执行成功，以返回为准；异步输出可能打断提示，可先`radar watch off`。语音命令不实现自动重试/事务队列；BLE只持久化bond和S3-FOB的last_seq。

## 验证

协议测试直接提取生产`voice.c`的编码器/解析器，RISC-V GCC编译后由Unicorn执行真实机器码。只替换硬件输出/打印，不重写协议算法。

```powershell
python -m pip install --target tests/.deps -r tests/requirements.txt
python tests/test_voice_host.py
```

11项测试覆盖手册帧编码、非法编码参数、全部拆分位置、粘帧/噪声、坏校验、坏尾、非法长度、负载内头尾、错误长度后超时恢复、过期残帧和257字节最大帧。它们不验证S3外设或FreeRTOS运行。

实机待验收：UART0 shell、PB4开漏低电平/释放、AT查询和检测开关、OUT及保持时间、语音查询/音量/播放/停止/BUSY、并发、错误参数、发码过快、串口错误后恢复。可用逻辑分析仪核对波特率、CRLF及HEX实际字节。未刷写或读取实机，不将构建和协议测试记为硬件通过。
