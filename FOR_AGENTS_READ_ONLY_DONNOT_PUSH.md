# SolarClock 项目档案（供 Agent 会话延续用）

> **本文件只读**：仅用于向新会话的 Agent 描述项目全貌与进度。
> 不要修改本文件，不要把它提交/推送到 git。
> 你读完它后，如需获取最新状态，请以实际文件与串口实测为准。

---

## 1. 项目是什么

SolarTime demo（仓库名 SolarClock）：**太阳历/太阳时系统**。

- 输入：GPS 经纬度 + 时区 + UTC 时间戳（一次获取）
- 核心：纯 C 天文算法，本地离线计算 **当地太阳时、日出/日落/正午、距日出/日落、日光进度**
- 平台：PC（C CLI 原型）→ ESP32-S3 固件（目标平台）
- 最终形态：GPS 芯片 → ESP32（算完转 RTC 离线计时）→ LCD/前端 UI 显示

**用户目标链**：GPS芯片发真实 ts+经纬度 → ESP32 按 `init` 格式算一次 → 转 RTC 计时 → 100ms 心跳帧 → LCD 渲染。UI 与算法代码**必须分离**，JSON 全程**纯 ASCII 无中文**。

## 2. 仓库现状

- 本地目录：`/home/lx/Documents/SolarTime`（git 仓库，remote 为 https://github.com/LxCenady/SolarClock.git）
- 硬件：ESP32-S3，`/dev/ttyACM0`，板子无原生 USB 口（USB-C 转接头内置 CH340 桥），控制台走 **UART0 → CH340**，115200
- ESP-IDF v6.0.2 位于 `/opt/esp-idf`，python env `idf6.0_py3.14_env`

```
solar.h/solar.c      核心算法（PC/ESP32 共用，esp32/main/ 下有同源码副本）
main.c               PC CLI: ./solartime set lat lon tz / ./solartime
esp32/               ESP-IDF 工程（idf.py build，target esp32s3，sdkconfig.defaults）
ui/                  TUI: main.py(solarclock入口) + link.py(串口协议) + render.py(纯ANSI渲染)
tools/               验证工具: gen_dataset / verify_dataset / hot_test / edge_test / inject_gps
                     数据集 dataset.json（40 样本，astral 参考值）
```

## 3. 串口协议（纯 ASCII JSON，键序无关解析）

请求（一行）：
```json
{"cmd":"solar","ts":<utc秒>,"lat":..,"lon":..,"tz":..}   // 一次性应答
{"cmd":"init", ...同上}                                  // 应答后进入心跳模式
{"cmd":"stop"}                                           // 退出心跳
{"cmd":"get"}                                            // 返回当前坐标
```
应答（solar/init 一次性）：
```json
{"rise":"05:24","noon":"11:56","set":"18:28","solar":"13:55",
 "decl":11.42,"eot":-2.7,"to_set":269,"polar":null}   // polar: null|"day"|"night"
```
心跳（100ms = 10Hz，init 之后持续）：
```json
{"t":"13:52:05","d":"08-26","s":"13:55","r":"05:24","st":"18:28",
 "ne":0,"tne":269,"dp":62,"ev":0,"p":0,"la":31.23,"lo":121.47}
```
- `ne`：0 白天(距日落) / 1 夜晚(距日出) / 2 极昼 / 3 极夜
- `tne`：距下一事件分钟（0-1439）
- `dp`：日光进度 %（极昼=100，极夜=0，区间外钳制）
- `ev`：0 无 / 1 日出事件 / 2 日落事件（±30s 窗口）
- `p`：0 正常 / 1 极昼 / -1 极夜；极区时 r/st 为 "--"
- 极区占位文案（UI 侧）：极昼 `The Sun is ALWAYS there.` / 极夜 `The Sun Rises, but NOT TODAY`
- 事件文案：日出 `It's the Sun, Again.` / 日落 `Another Day Has Gone`

## 4. 算法要点（solar.c）

- **简化 NOAA 算法**（Meeus 方程）：儒略日 → 太阳赤纬 + 均时差
- 日出/日落天顶角 **ZEN = 90.833**（NOAA 官方标准 = 0.267 太阳半径 + 0.567 地平折射）
- **事件时刻赤纬精化**（2 次迭代，与参考一致；实测 1 次已收敛）
- **浮点精度加固**：`atan2(√(1-c²),c)` 替代 `acos(c)`（c≈±1 极区边界条件数远优于 acos）；日出/日落/正午保存**亚分钟小数**（SolarResult 的 rise_min 等为 double），事件在精确秒触发；显示一律四舍五入
- 边界加固：`fmod` 负值归一化（东经+负时区曾误判极区）；极区判定 ε=1e-9；c 钳制 ±1 防 NaN；纬度钳制 ±89.8°
- **验证基准 = NOAA 官方算法**（tools/noaa_ref.py，独立实现无精化），astral 降级为 `--ref astral` 交叉复核
- 容差（对 NOAA 基准）：|lat|≤60° → 2min；≤72° → 6min；>72° → 12min；>72° 极区标志不一致降级为警告
- 注：astral 折射约定（90.789°）与 NOAA 标准（90.833°）差 0.044°，极端纬度/极圈边界有分钟级差异，属 astral 模型约定而非本算法 bug

## 5. 固件状态机

```
命令任务(cmd, 钉CPU1): 空闲→组行→解析(cmd/init/stop/get)→应答
主循环(CPU0): 空闲轮询100ms; g_hb=1 时每100ms: solar_compute→send_hb
日期变更: 每次心跳重新计算, 自动滚动到新一天的日出日落
```

## 6. TUI（solarclock 命令，已装 ~/.local/bin/solarclock）

```
solarclock                   默认GNSS监听模式(等硬件定位)
solarclock --use-cache       使用 ~/.solarclock.json 缓存坐标/时间偏移注入
solarclock -c                交互配置 坐标/时区/当地时间 → 存 ~/.solarclock.json(delta_h 时间偏移)
solarclock -r                随机陆地经纬度+时区(timezonefinder)
solarclock --local           PC 本地 IP 定位(ipinfo.io/ip-api)打包 JSON 注入
solarclock -L lat -O lon -T tz [--ts 秒]   显式指定(测试用)
q 退出；状态机 DISCONNECTED→INIT→LIVE→QUIT
打开串口后等待2s(触发复位), 再发 init；search JSON 仅在 !g_hb 时输出
```
渲染帧（8 行固定，光标归位无闪烁，进入 LIVE 时清屏一次）：
```
 SolarClock  08-28  19:48:26
 solar time  17:15
 location    31.2304N 121.4737E
 sunrise     05:27
 sunset      18:22
 daylight    [##################--]  90%
 next event  sunset in 71 min   (夜晚显示 sunrise in)
 event       (It's the Sun, Again. / Another Day Has Gone / 空)
```

## 7. 验证体系（改算法/固件后必须全量回归）

```bash
python3 tools/verify_dataset.py        # 40 样本固定数据集 (基准: NOAA)
python3 tools/edge_test.py             # 14 边界用例(日期变更线/极圈/极点/半时区/午夜)
python3 tools/hot_test.py --rounds 30 --seed N          # 随机经纬度+时区反查+热验算 (基准: NOAA)
python3 tools/hot_test.py --ref astral                  # astral 交叉复核(仅参考)
```
参考实现 tools/noaa_ref.py = NOAA 官方算法（90.833 + 无精化）；astral 仅作交叉复核
（其极昼/极夜抛 ValueError 需 try/except；`sun_declination`/`eq_of_time` 参数是**儒略世纪**）。
工具打开串口后必须先发 `{"cmd":"stop"}` 清场，并等 2s（打开串口会触发复位）。

## 8. 已踩过的坑（不要重踩）

1. **v6 控制台默认无 uart 驱动**：`uart_read_bytes` 返回 `ESP_ERR_INVALID_STATE`，必须显式 `uart_driver_install(UART_NUM_0,512,0,0,NULL,0)`
2. **stdio getchar 与心跳 printf 并发** → task watchdog（IDLE0 饿死）；用 `uart_read_bytes(...,5ms超时)` 轮询，命令任务钉 CPU1
3. **应答与心跳 printf 行内交错** → 输出互斥锁 `s_out`（xSemaphoreCreateMutex）
4. `fgets` 在 raw 模式立即返回 NULL → 逐字符组行解析
5. 东经 180° + 负时区 → `fmod` 负值 → 误判极区（已修）
6. 无 `-c`/缓存时默认坐标从 ESP32 读（`get` 命令）
7. astral 的 `sun()` 聚合函数在极区崩溃，用 `sunrise/noon/sunset` 单函数 + ValueError 兜底
8. **DS3231 写入帧结构（审计抓出，硬件未到货时完全不可见）**：I2C 写帧第一字节必须是寄存器指针——
   - 原实现 `i2c_master_transmit(w+1, 6)` 开头直接是分钟值（被当指针，全部错位）
   - 秒的最后写入只发 1 字节（只有地址无数据，秒从未真正写入）
   - 修复: `first[7]={0x01,min,hour,dow,mday,mon,year}` + `sec_frame[2]={0x00,sec}`；世纪位(年≥2100)置月寄存器 BIT7
   - **教训: 任何 I2C/串口帧构造都要写成 [指针][数据] 并做字节级单测，读路径 transmit_receive 复核无此问题**
9. **固件全面审计(2026-08-29, subagent) → 5 必修 + 4 顺手修**：
   - B1 dp跨午夜恒0(已修fmod回绕) / B2 get/stop应答无锁(已修) / B4 ev事件fmod负侧漏判(已修if/else回绕) / B5 g_cfg跨任务撕裂(写侧加锁+读侧锁内副本)
   - 顺手: tne钳制1439、主循环单次now、gnss空字段校验、to_set注释
   - 审计误报1项: hm()静态缓冲实为锁内调用(B3不成立)

> **⚠️ 重要警告**: 固件/算法中**仍可能存在未被发现的逻辑 bug**（硬件未到货，
> 大量路径从未在真实硬件上跑过）。后续接入 GNSS/RTC/LCD 或出现任何异常
> （时间错乱/心跳丢失/数据异常/偶发崩溃）时，**必须优先考虑代码逻辑 bug 的可能性**，
> 而不是先怀疑硬件。排查手段: 看门狗回溯+addr2line、串口JSON抓包、单测回归套件。

## 10. 到货前测试能力（已就绪, 无需硬件）

- `tools/test_gnss_parser.py`: GNRMC解析 6/6（C vs 独立Python交叉验证）
- `tools/test_rtc_frames.py`: RTC写帧字节级 5/5（rtc_core纯逻辑层）
- `tools/gnss_sim.py`: GNSS全链路模拟 4/4（nmea注入命令→handle_fix→心跳, 与真实链路同代码路径）
- `tools/pcas_config.py`: $PCAS校验和生成(官方样例验证)+setup一键初始化+tx1转发
- 固件命令: `nmea`注入 / `tx1`转发(需CONFIG_SOLAR_GNSS)

## 11. 零号机（硬件到货, 全链路闭环 2026-09-02）

**接线（已实测通过）**:
- GNSS ATGM336H-5N-31: TXD0→GPIO17(U1RXD), RXD0→GPIO18(U1TXD), VCC 3.3V, GND; **波特率 9600**(5N默认, 非6N的115200); 必须接天线(有源)
- RTC DS3231: SCL→GPIO9, SDA→GPIO8, VCC 3.3V, GND; **无VBAT脚**(断电即丢时间, 靠GNSS对时); SQW脚可选
- ESP32-S3 经 CH340 USB 转接(枚举 1a86:55d3, ttyACM0)

**实测结果**:
- GNSS 室外(泳池边)定位成功: 坐标 1.3593N 103.7660E(新加坡), 1Hz fix → 心跳模式
- RTC DS3231: I2C 0x68 探测OK, 写入+OSF清除, 心跳时间源=RTC走时正常(与PC差<2s)
- 全链路: GNSS→$GNRMC解析→防抖→handle_fix→RTC对时→solar算法→100ms心跳→TUI

**关键设计/决策**:
- 固件开机自动下发 $PCAS 配置(GGA+RMC+保存) + **$PCAS10,2 冷启动**: 每次上电强制重新搜星
- TUI `solarclock` 默认 = **GNSS监听模式**(不注入init, 直接等硬件定位); 搜星过程显示 V帧/可见卫星数
- 搜星状态JSON(1Hz): `{"gnss":"search","v":N,"sats":S}`; GGA解析卫星数诊断
- 时间源优先级: DS3231(RTC) > GNSS帧 > PC注入; 注入ts会同步写RTC
- 符号冲突坑: rtc_init 与 IDF esp_hw_support 内置 rtc_init 冲突 → 已改名 ds3231_*

**遗留待办**:
1. **室内判定逻辑**: 无fix时用缓存坐标+RTC继续跑心跳(而非停在搜星界面), 标记定位丢失状态(用户已提出)
2. LCD渲染层(消费心跳JSON, 替换ui/render.py)
3. 独立供电(LDO, GNSS纹波<50mVpp)

## 11b. LCD (ST7789 240x240 SPI) 已接入 (2026-09-03)

**接线**: SCK=GPIO11 MOSI=GPIO12 DC=GPIO13 RES=GPIO14 CS=GPIO15 BLK=GPIO16; VCC/GND走面包板电源轨(3.3V/GND与其他芯片共用); 避开RTC(8/9/10) GNSS(17/18) USB(19/20)
**Kconfig**: CONFIG_SOLAR_LCD + SOLAR_LCD_PIN_*(默认11-16)

**关键经验(颜色/字体坑)**:
- ST7789 颜色: rgb_ele_order=RGB; **所有颜色值必须经 rgb565() 做字节交换**(SPI高字节先发); 直接用裸uint16常量会绿红互换
- 字体: 5x7放大2倍(10x14), 行高16; 每行字符上限 240/12=20字符, 长文本会溢出右侧(需截断/精简标签)
- esp_lcd_panel_swap_color_bytes API不存在, 字节序在rgb565()内手动处理
- esp_lcd_panel_io: SPI2_HOST + DMA, esp_lcd_panel_st7789组件

**LCD状态流(简洁版, 无动画)**: 上电黑屏 → 搜星界面(GPS ACQUIRING/V帧/SATS, 1Hz) → fix后直接心跳面板(100ms)
- lcd.c: 自绘5x7字体(lcd_font.h)放大渲染, rgb565字节交换, json_get简易JSON字段提取
- lcd_render_hb(心跳JSON) / lcd_render_search(V帧,卫星数)
- gnss_task V帧时调lcd_render_search, send_hb调lcd_render_hb

**遗留**: 背光BLK现直连GPIO16高电平(全亮), 后续可PWM调光

## 11c. 充电宝独立运行 (2026-09-03, 室外实测通过)

**用户实测**: 拔USB用充电宝独立跑, GNSS红灯1Hz稳定闪烁(持续定位), 离线计算/显示全部正常 → **零号机独立运行确认**

**新增逻辑**:
- **NVS坐标持久化**: 首次fix存NVS(仅坐标移动>0.001°才写, 防flash磨损); 开机读NVS覆盖menuconfig默认
- **RTC信任判定** `rtc_trusted()`: OSF=0 且 year∈[2024,2099] 才算可信(防出厂默认2000年)
- **RTC可信 → 直接进心跳**: 充电宝持续供电时室内无fix也能当正常时钟(缓存坐标+RTC时间); GNSS后台继续搜星
- **LCD渲染1Hz降频**(原100ms) + 搜星渲染仅在 !g_hb 时画(防与心跳面板打架)
- **接电即显示**: RTC不可信时上电立即 lcd_render_search(0,0), 不等GNSS首帧(消除冷启动黑屏空窗)

**行为矩阵**:
| 场景 | 行为 |
|---|---|
| 充电宝供电室内开机(RTC保持) | 缓存坐标+RTC直接时钟主界面, GNSS后台搜星 |
| 户外fix | 自动更新坐标+NVS保存+RTC对时 |
| 真断电后开机(RTC丢) | 接电即显示搜星画面, fix后切时钟 |

**功耗现状(未优化)**: GNSS常开+LCD背光全亮(直连GPIO16)+心跳UART 100ms输出; 后续: LCD PWM调光/空闲降频/GNSS周期唤醒/深睡

## 11d. 全量审计修复 (2026-09-03)

**固件逻辑修复**:
- **S1 高危: `cmd:"solar"` 不再落状态** — 之前 solar 一次性计算会写 `g_cfg`/`g_t0`，RTC在线时还会把 DS3231 写成请求里的任意历史/随机 ts（验证套件会污染 RTC）。现在 solar 纯计算；仅 `init` 更新配置/时间基准并同步 RTC。
- **S2 并发: 64位 `g_t0` 与 `g_cfg` 撕裂** — `now_t()` 读 `g_t0` 加 `s_out` 互斥；`handle_fix` 在锁内同时写 `g_cfg`+`g_t0`；`cmd_task`/`handle_fix` 均用局部 cfg 副本参与 `solar_compute`，不再无锁读全局。
- **S3 GNSS 解析加固** — `gnss_parse_rmc` 新增 -7 返回码：半球字段缺失/非法、坐标越界(±90/±180)、时间日期域值非法；`handle_fix` 二次校验坐标范围；防抖时间差改用 `llabs`。
- **S4 LCD 互斥与空指针** — 新增 `s_lcd_mutex` 串行化 search/hb 渲染；渲染前检查 `s_panel && s_buf && s_lcd_mutex`。
- **S5 串口/JSON 健壮性** — 命令与 NMEA 行超长丢弃至换行，不再截断解析；`json_num` 拒绝 NaN/inf；`pcas_send` 检查 snprintf 截断。
- **S6 配置** — `SOLAR_TZ`/`SOLAR_GNSS_TZ` 改为 float 支持半时区；`SOLAR_GNSS_BAUD` 默认 9600(与5N零号机一致)；CMake 显式 `esp_driver_i2c`。

**工具/TUI 修复**:
- `pcas_config.py setup` 改为 GGA+RMC（原 only_rmc 会关 GGA 导致 sats 恒 0）。
- `hot_test.py`/`verify_dataset.py` 极区用例现在校验 `polar` day/night，不再只校验“是否极区”。
- `test_gnss_parser.py` 新增 -7 用例(9/9)，`test_rtc_frames.py`(5/5) 均自动建 `/tmp/opencode`。
- `ui/main.py` 打开串口等 2s 并清缓冲；新增 `--use-cache` 让 `-c` 缓存真正可用。

**回归**: `idf.py build` 通过；PC `gcc -Wall` 通过；gnss/rtc 单测通过。

## 12. 下一步（用户计划）

1. **硬件阶段转入用户亲自主导**: 优化布线(面包板→成品), 后期**光伏供能**
2. 软件遗留: 无fix时的"定位丢失"标记显示(部分已由缓存坐标+RTC方案覆盖); LCD PWM调光; 功耗优化
3. 用户习惯：每次推送需提供 classic GitHub PAT（`repo` scope），用完即吊销；token 用过即从 remote URL 清除

## 13. 提交历史（main 分支）

```
(待提交) 全量审计修复: solar纯计算/RTC防污染/并发互斥/GNSS校验/LCD互斥/TUI与工具修复
(待提交) 独立运行: NVS坐标持久化 + RTC信任自动心跳 + LCD降频/接电即显示
540cd3d  LCD: ST7789驱动+放大字体+搜星界面, 无动画
c5158d4  零号机: GNSS数据源切换+搜星显示+每次冷启动; GGA卫星数; TUI默认GNSS监听
2cef591  固件审计修复 + rtc_core帧单测 + nmea注入/gnss_sim + pcas工具
525b8cc  GNSS-RTC占位协议: GNRMC解析+DS3231骨架+Kconfig集成+协议文档
dd688f4  浮点精度 + NOAA基准切换
9583585  昼夜判定ne/tne; 输出互斥锁; 工具清场; TUI标签大写
...（更早: 算法/JSON协议/数据集/基础算法）
```
