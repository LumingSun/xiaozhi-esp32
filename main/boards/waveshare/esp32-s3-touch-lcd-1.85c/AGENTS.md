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

### 2. 触摸当前不可用（已知问题）

**现象**：`CST816S` 芯片 ID 能读到（`IC id: 181` = 0xB5），但数据寄存器 `0x02` 的读
**100% NACK**，140 次/秒。因此探针式支持不了触摸。

**已排除**：`int_gpio_num` 配置（`GPIO_NUM_NC` 与 `TP_PIN_NUM_INT`=GPIO4 都试过）、
I2C 总线共享（与 TCA9554/ES8311/ES7210 同总线）、上电复位时序（TCA9554 EXIO1 已正确
复位）。

**下一个未验证怀疑**：I2C 400kHz 对本板触摸走线过快（1 字节 ID 读成功、6 字节数据读
失败，符合信号完整性临界）。试法：`tp_io_config.scl_speed_hz` 改 `100 * 1000`。

**现状**：`InitializeTouch()` 末尾有 `ESP_LOGW(...) + return;`，**故意不注册输入设备**
（避免 140 次/秒失败事务刷屏 + 占 CPU）。触摸恢复后删掉这两行即可——下面是完整的
**容错 indev**（绝不用 `ESP_ERROR_CHECK` 包读操作），不要退回 `lvgl_port_add_touch`
（它内部的 `lvgl_port_touchpad_read` 用 `ESP_ERROR_CHECK` 包着读，一次 NACK 就 abort）。

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

## 验收（改完必做）

1. 烧录后抓 20-30s 日志，确认：**无 `Guru Meditation`、无 `ESP_ERROR_CHECK failed`、
   `unknown -> starting` 只出现 1 次**
2. 连上 WiFi 后等 ≥5s，确认 `StateMachine: activating -> idle` 走完
3. 空闲 60s 看信息屏是否出现（时间/日期/天气）
4. 报告里必须注明「已验证 / 待真机验证」，**编译通过不等于可用**
