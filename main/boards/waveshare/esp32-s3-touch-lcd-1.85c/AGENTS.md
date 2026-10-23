# AGENTS.md — Waveshare ESP32-S3-Touch-LCD-1.85C

本目录是本板唯一实现。**本文件规则在 `main/boards/waveshare/esp32-s3-touch-lcd-1.85c/`
子树内优先级最高**，补充上级 `xiaozhi-esp32/AGENTS.md`（上游文件，勿改）与
`~/esp/AGENTS.md`。

## 双变体结构

同一硬件、同一 `board.type`（`esp32-s3-touch-lcd-1.85c`，OTA 兼容），两个构建变体：

| 变体 | Kconfig | Display 类 | 构建名 |
|---|---|---|---|
| stock（默认） | `WAVESHARE_..._HOME=n` | `SpiLcdDisplay` | `esp32-s3-touch-lcd-1.85c` |
| home | `WAVESHARE_..._HOME=y` | `HomeLcdDisplay` | `esp32-s3-touch-lcd-1.85c-home` |

差异全部走 `#ifdef CONFIG_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85C_HOME`，**原版行为不得改变**。
`home_display.{h,cc}` 只放 home 变体的显示逻辑，不要往里塞板级 pins 或核心模块行为。

## 必须遵守的坑位规则

### 1. LVGL 字体生命周期（本板头号杀手）

**（已导致 3 次真机重启循环，务必读懂再动 display 代码）**

LVGL 样式只存裸 `lv_font_t*`。`Assets::Apply()` 解析到 assets 里的 `text_font` 后会调
`SetTextFont()` 换主题字体；`LvglDisplay::SetTextFont` 的逻辑是：

```cpp
auto previous_light_font = light_theme->text_font();  // 先持有旧字体
light_theme->set_text_font(new_font);
SetTheme(current_theme_);                             // 重绑「主题样式」
previous_light_font.reset();                          // 再释放旧字体
```

它只保证**主题管理的样式**被重绑。**用 `lv_obj_set_style_text_font` 设的局部样式不在
重绑范围内**——旧字体一释放，这些对象就持有悬空指针，下一次布局刷新调
`lv_font_get_glyph_width → lv_font_get_glyph_dsc → f->get_glyph_dsc` 时空指针 →
`Guru Meditation InstrFetchProhibited PC=0x0`。

**规则**：给自建控件设局部字体时，**必须**把 `shared_ptr<LvglFont>` 存成显示类的成员：

```cpp
// home_display.h
std::shared_ptr<LvglFont> info_text_font_;
std::shared_ptr<LvglFont> info_icon_font_;

// home_display.cc (BuildInfoScreen)
info_text_font_ = text_font;   // ← 持有所有权，对象活到本类析构
lv_obj_set_style_text_font(lbl, info_text_font_->font(), 0);
```

另外：**字体指针非 NULL ≠ 可用**。`font()->get_glyph_dsc` 也要判（`LvglCBinFont::font()`
可能非空但回调为空）。`BuildInfoScreen()` 开头有两层校验 + fail-soft 跳过，勿删。

### 2. 触摸：CST816T 自动休眠（2026-10-23 已定位并修复）

**根因**：本板触摸芯片 ID 是 **0xB5 = CST816T**（不是 CST816S 的 0xB4）。实测其
`0xFE`(DisAutoSleep)=`0x00`、`0xF9`(autosleep time)=`0x00` —— **自动休眠是开的**。
CST8xx 进入 auto-sleep 后**不再响应任何 I2C 读**（Zephyr `cst8xx` 驱动注释里也记录了
这条行为），于是：上电复位后 ID 能读到，随后所有读（含 `0xA7` 芯片 ID）持续 NACK，
140 次/秒。**和速率、长度、寄存器、信号完整性都无关**。

**诊断实证**（2026-10-23，一次性诊断固件）：`0x00–0xFF` 全扫 256 次全部 OK → 空闲数百
毫秒后再读全 NACK（100k 也 NACK）→ 写 `0xFE=0x01` 后 `0x02`/`0xA7` 立即恢复且不再失败。

**修复**：`InitializeTouch()` 里驱动初始化后追加

```cpp
TouchWriteReg(tp_io_handle, 0xFE, 0x01);   // 必须！espressif 官方驱动不写这一位
```

并读回 `0xFE` 校验。读回调在持续 NACK 时会自动重写该寄存器自愈（写事务能到达睡死的
芯片，这是唯一的唤醒手段）。

**不要退回 `lvgl_port_add_touch`**：它内部的 `lvgl_port_touchpad_read` 用
`ESP_ERROR_CHECK` 包着读，一次 NACK 就让整机 abort（2026-10-23 曾因此重启循环）。
当前自建 indev 直接读 `0x02` 的 6 字节并手工解析（手指数/XY），读失败只当作"本次无触摸"。

### 3. I2C 拓扑（别踩错总线）

- 主总线：`I2C_NUM_0`，SDA=GPIO11，SCL=GPIO10（`InitializeI2c()` 创建 `i2c_bus_`）
- 同总线挂：TCA9554（`I2C_ADDRESS`，复位 LCD+Touch）、ES8311、ES7210、CST816S
- `config.h` 里的 `TP_PORT (I2C_NUM_1)` 与 `TP_PIN_NUM_*` 是**上游微雪 demo 的遗留定义**，
  本板实现只用了 `TP_PIN_NUM_INT`
- TCA9554 的 EXIO0 复位 LCD、EXIO1 复位触摸；`InitializeTca9554()` 已做
  `高 300ms → 低 300ms → 高`，**不要在别处再动 EXIO1**

### 4. 构建/烧录（必须重新归档）

`~/esp/xiaozhi-home/scripts/flash_firmware.sh` 烧的是 `~/esp/firmware-builds/` 的**归档**，
**不是 `build/`**。编译完不 `cp` 归档 = 烧到旧固件（已实际踩坑，白费两轮调试）。

单条命令搞定（编译 → 归档 → 烧固件 → 烧资产）：

```bash
DEST=~/esp/firmware-builds/esp32-s3-touch-lcd-1.85c-home
. ~/esp/env.sh && cd ~/esp/xiaozhi-esp32 && \
  python3 scripts/build.py waveshare/esp32-s3-touch-lcd-1.85c \
    --name esp32-s3-touch-lcd-1.85c-home --language zh-CN && \
  cp build/bootloader/bootloader.bin build/partition_table/partition-table.bin \
     build/ota_data_initial.bin build/generated_assets.bin build/xiaozhi.bin \
     build/flasher_args.json "$DEST/" && \
  bash ~/esp/xiaozhi-home/scripts/flash_firmware.sh home && \
  python3 -m esptool --port /dev/cu.usbmodem1101 write_flash 0x800000 \
     ~/esp/xiaozhi-home/ui-assets/assets.bin
```

分区表是 `partitions/v2/16m.csv`：`ota_0` 0x20000+0x3f0000、`ota_1` 0x410000+0x3f0000、
**`assets` 0x800000 起 8MB**。`generated_assets.bin` 与定制 `assets.bin` 都落这个分区，
后者覆盖前者（定制包已含唤醒模型）。

### 5. 抓日志（别用 idf.py monitor）

`idf.py monitor` **默认复位板子**，且被 `timeout` 杀掉时可能把虚拟 DTR 摁住，导致之后
所有复位都进 download 模式（`boot:0x0`，软复位无效，只能拔插断电）。

```bash
python3 -c "
import serial,time,sys
s=serial.Serial('/dev/cu.usbmodem1101',115200,timeout=1)
e=time.time()+30
while time.time()<e:
    d=s.read(8192)
    if d: sys.stdout.write(d.decode('utf8','replace')); sys.stdout.flush()"
```

烧录后板子会自己重启，**紧接着跑上面这条**就能抓到完整启动日志。

### 6. home 变体的定时/状态参数

`home_display.cc` 顶部常量：`kStateTickMs=1000`、`kActiveToInfoMs`（60s）、
`kInfoToScreenOffMs`（300s）、`kWeatherStartDelayMs=15s`、`kWeatherRefreshMs=30min`、
`kWeatherRetryMs=60s`、`kWeatherTaskStack=8192`。

- 状态机 tick 由 **LVGL timer** 驱动（跑在 LVGL 任务内，无需额外加锁）
- 天气任务跑在**独立 FreeRTOS 任务**，`FetchWeather()` 拉 `wttr.in`（实测约 40KB，
  流式累计、64KB 上限）；结果通过 `Application::Schedule` 排到主任务再改 LVGL
  ——**这条路径勿改成直接在天气任务里改 UI**
- 天气城市：`menuconfig → ESP32-S3-Touch-LCD-1.85C home variant → Weather city`

### 7. 空闲状态机：状态栏时钟会重置空闲计时（信息屏不出）

**（2026-10-23 真机定位，症状：等再久也进不了信息屏）**

`LvglDisplay::UpdateStatusBar()` 由 `Application` 每秒调用一次，其中在**空闲态每分钟整点**
会调 `display->SetStatus(当前时间)` 刷新状态栏时钟。而 `HomeLcdDisplay` 把
`SetStatus/SetEmotion/SetChatMessage` 的"非空即活动"当作亮屏触发 → 空闲计时被每分钟重置，
`kActiveToInfoMs`（60s）永远达成不了。实测 idle 每次涨到 ~55s 就被打回 0。

**规则**：`SetStatus/SetEmotion/SetChatMessage` 只有处于**交互语境**时才亮屏/重置计时——
判据是 `Application::GetInstance().GetDeviceState() != kDeviceStateIdle`
（见 `home_display.cc` 的 `IsInteractiveContext()`）。空闲态下只接受
**触摸（屏幕按压/音量亮度按钮）** 与 **语音（状态机切出 idle）** 作为唤醒来源。

诊断手法（已固化经验）：在 `OnStateTick()` 里每 5s 打印 `state` + `idle`，在
`WakeAndActivate()`/`OnUserActivity()` 入口打印调用来源（`SetStatus`/`SetEmotion`/
`screen-press`…）。这次就是靠"来源=SetStatus 且间隔恰好 60s"一眼定位的。

## 验收（改完必做）

1. 烧录后抓 20-30s 日志，确认：**无 `Guru Meditation`、无 `ESP_ERROR_CHECK failed`、
   `unknown -> starting` 只出现 1 次**
2. 连上 WiFi 后等 ≥5s，确认 `StateMachine: activating -> idle` 走完
3. 空闲 60s 看信息屏是否出现（时间/日期/天气）
4. 报告里必须注明「已验证 / 待真机验证」，**编译通过不等于可用**
