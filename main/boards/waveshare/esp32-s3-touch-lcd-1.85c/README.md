新增 微雪 开发板: ESP32-S3-Touch-LCD-1.85C
产品链接：
https://www.waveshare.net/shop/ESP32-S3-Touch-LCD-1.85C.htm

## home 变体（家庭助手定制 UI）

同一硬件、同一 `board.type`（`esp32-s3-touch-lcd-1.85c`，OTA 兼容性不变）的
构建变体：通过 Kconfig 开关 `WAVESHARE_ESP32_S3_TOUCH_LCD_1_85C_HOME` 把 Display
换成 `HomeLcdDisplay`（`home_display.h/.cc`），用于小智家庭助手的触摸 UI 定制。
原版构建行为完全不变（默认关闭）。

构建命令：

```bash
# home 变体（触摸 UI 定制骨架）
python3 scripts/build.py waveshare/esp32-s3-touch-lcd-1.85c --name esp32-s3-touch-lcd-1.85c-home --language zh-CN

# 原版回归
python3 scripts/build.py waveshare/esp32-s3-touch-lcd-1.85c --name esp32-s3-touch-lcd-1.85c --language zh-CN
```

注意：`--name esp32-s3-touch-lcd-1.85c-home` 会作为 OTA 上报的 `board.name`
（全局唯一）；自建服务端按设备 MAC 识别，新 name 属预期。
