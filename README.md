# STM32F407 Tetris 项目文档

## 1. 项目概述

本项目基于正点原子 STM32F407 探索者 V3 开发板实现一个带图形界面、背景音乐、历史记录和硬件状态反馈的俄罗斯方块小游戏。项目最初以基础 Tetris 玩法为核心，后续结合开发板资源扩展了 2.8 寸 LCD 适配、BGM 播放、EEPROM 记录保存、RTC 时间戳、LED 状态反馈和 SPI FLASH 存储等功能。

当前版本重点面向 2.8 寸小尺寸 LCD 屏幕进行了 GUI 适配和界面优化，支持在开始菜单查看历史分数、调整 BGM 音量，并在 Game Over 后返回开始菜单继续查看记录或修改设置。

## 2. 功能特性

### 2.1 游戏功能

- 标准 10 列 x 20 行俄罗斯方块棋盘。
- 支持 7 种经典方块：I、T、S、Z、J、L、O。
- 支持方块左移、右移、旋转、硬降。
- 支持下一方块预览。
- 支持影子方块，用于提示当前方块硬降落点。
- 支持消行计分、等级提升和下落速度递增。
- 支持 Game Over 结算页面。
- Game Over 页面支持：
  - `KEY0` 直接开始新游戏。
  - `KEY_UP` 返回开始菜单。

### 2.2 GUI 功能

- 适配 2.8 寸 240x320 竖屏 LCD。
- 已备份旧大屏幕 UI 参数到 `Core/Inc/tetris_ui_large_backup.h`。
- 当前小屏 UI 使用紧凑布局：
  - 左侧主棋盘。
  - 右侧 NEXT、SCORE、LINES、LEVEL 信息区。
  - 底部按键说明。
- 开始菜单包含：
  - 游戏标题。
  - BGM 音量条。
  - 最高分显示。
  - 游戏按键说明。
  - 历史记录入口。
- EEPROM 历史页支持分页显示最近游戏记录。
- Game Over 页面显示本局分数、消除行数、最高分，并提供重开和返回菜单操作。
- 方块绘制加入高光、阴影和内层填充，使视觉效果更清晰。
- 消行动作加入短暂闪烁反馈。

### 2.3 音频功能

- 使用板载 ES8388 音频 Codec 和扬声器播放 BGM。
- BGM 使用 `Korobeiniki.mp3` 转换生成的 PCM C 数组。
- 当前音频参数：
  - 采样率：8000 Hz。
  - 声道数：单声道源数据，播放时复制为双声道。
  - PCM 数据文件：
    - `Core/Inc/korobeiniki_pcm.h`
    - `Core/Src/korobeiniki_pcm.c`
- 播放方式：
  - I2S2 主机发送。
  - DMA 循环传输。
  - 半传输和全传输回调中填充音频缓冲区。
- 菜单音量范围为 0..10 档。
- 实际输出音量限制为原始 PCM 音量的 0%..3%，避免板载扬声器过响。
- 音量设置会保存到 EEPROM，复位后保持上次设置。

### 2.4 EEPROM 设置和历史记录

EEPROM 用于保存用户设置和游戏历史。由于板载 EEPROM 容量较小，当前实现采用紧凑结构保存最近 10 次游戏记录。

保存内容包括：

- 历史最高分。
- 历史最高消除行数。
- BGM 音量设置。
- 最近 10 次游戏记录：
  - 分数。
  - 消除行数。
  - 游戏持续时间。
  - 开局 RTC 时间。

EEPROM 记录具备以下保护机制：

- `magic` 标记用于识别有效记录。
- `version` 用于处理记录格式升级。
- `size` 用于检查结构体大小是否匹配。
- `checksum` 用于校验数据完整性。
- 写入后会读回校验，失败时自动重写一次。

### 2.5 RTC 时间功能

RTC 用于给每一局游戏历史记录添加时间戳。

RTC 初始化策略：

- 优先使用 LSE 外部低速晶振。
- 如果 LSE 启动失败，自动回退到 LSI 内部低速时钟。
- 使用备份寄存器 `RTC_BKP_DR0` 保存 RTC 初始化标记。
- 如果备份标记不存在，说明 RTC 尚未设置，系统会使用固件编译时间作为默认时间。

相关接口：

- `App_RTC_Init()`
- `App_RTC_GetDateTime()`
- `App_RTC_SetDateTime()`
- `App_RTC_IsRunning()`

当前版本已经具备 RTC 设置接口，但暂未提供独立的图形化校时菜单。如果需要精确时间，可后续新增菜单页面或串口命令进行手动校时。

### 2.6 LED 状态反馈

使用开发板 LED0 和 LED1 反馈游戏状态。

状态对应关系：

| 游戏状态 | LED 表现 |
| --- | --- |
| 开始菜单 | LED1 慢闪，LED0 关闭 |
| 游戏进行中 | LED1 周期闪烁，LED0 关闭 |
| 消行 | LED0/LED1 快速闪烁短暂提示 |
| Game Over | LED0 快速闪烁，LED1 关闭 |

LED 任务采用非阻塞状态机实现，不影响游戏刷新和按键响应。

### 2.7 SPI FLASH 功能

外部 SPI FLASH 当前用于保存最近一局游戏快照。

保存内容：

- 本局分数。
- 本局消除行数。
- 本局等级。
- 当前 BGM 音量。

实现方式：

- 使用软件 SPI，避免与 I2S/SPI2 音频播放通路冲突。
- 用户数据区域起始地址为 `0x100000`。
- 保存前擦除所在扇区。
- 使用 `magic + checksum` 验证记录有效性。

当前 BGM 仍以 C 数组形式存放在片内 Flash 中，SPI FLASH 中预留了 `SPI_FLASH_BGM_ADDR`，后续可扩展为从外部 FLASH 或 U 盘加载音频。

## 3. 硬件资源使用

### 3.1 主控与屏幕

- MCU：STM32F407。
- LCD：2.8 寸 TFT LCD。
- 显示方向：竖屏。
- 分辨率：240x320。
- LCD 驱动接口：FSMC。

### 3.2 按键映射

按键定义来自 `Core/Inc/main.h`：

| 按键 | GPIO | 菜单功能 | 游戏功能 | Game Over 功能 |
| --- | --- | --- | --- | --- |
| KEY0 | PE4 | 开始游戏 | 右移或开始相关逻辑中使用 | 重新开始 |
| KEY1 | PE3 | 音量减 / 历史页上一页 | 硬降或按键逻辑中使用 | 无 |
| KEY2 | PE2 | 音量加 / 历史页下一页 | 左移或按键逻辑中使用 | 无 |
| KEY_UP | PA0 | 打开/关闭历史记录页 | 旋转或硬降逻辑中使用 | 返回开始菜单 |

说明：当前工程中部分按键变量名与实际游戏动作存在历史适配关系，最终以屏幕提示和 `tetris.c` 中按键处理逻辑为准。

### 3.3 音频接口

- 音频 Codec：ES8388。
- I2S 外设：SPI2/I2S2。
- DMA：DMA1 Stream4。
- 软件 I2C 配置引脚：
  - SCL：PB8。
  - SDA：PB9。

### 3.4 EEPROM 接口

- EEPROM 地址：`0xA0`。
- 软件 I2C 引脚：
  - SCL：PB8。
  - SDA：PB9。
- EEPROM 页大小：8 字节。
- EEPROM 容量：256 字节。

注意：EEPROM 和 ES8388 共用 PB8/PB9。当前实现中 ES8388 仅在音频初始化时使用软件 I2C 配置寄存器，EEPROM 在保存和读取设置时复用同一组引脚。

### 3.5 SPI FLASH 接口

软件 SPI 引脚：

| 信号 | GPIO |
| --- | --- |
| CS | PB14 |
| SCK | PB3 |
| MISO | PB4 |
| MOSI | PB5 |

### 3.6 LED

- LED0：用于 Game Over 和消行提示。
- LED1：用于菜单和游戏进行中状态提示。
- 正点原子开发板 LED 通常为低电平点亮，当前代码中 `LED_ON` 定义为 `GPIO_PIN_RESET`。

### 3.7 RTC

- 优先使用 LSE。
- LSE 不可用时自动使用 LSI。
- 使用 RTC 备份寄存器保存初始化标记。

## 4. 软件架构

### 4.1 目录结构

```text
STM32F407_Tetris
├── Core
│   ├── Inc
│   │   ├── app_rtc.h
│   │   ├── app_settings.h
│   │   ├── audio_bgm.h
│   │   ├── korobeiniki_pcm.h
│   │   ├── led_feedback.h
│   │   ├── spi_flash.h
│   │   ├── tetris.h
│   │   └── tetris_ui_large_backup.h
│   └── Src
│       ├── app_rtc.c
│       ├── app_settings.c
│       ├── audio_bgm.c
│       ├── korobeiniki_pcm.c
│       ├── led_feedback.c
│       ├── spi_flash.c
│       ├── tetris.c
│       └── main.c
├── Drivers
├── cmake
├── example
├── CMakeLists.txt
├── STM32F407_Tetris.ioc
├── Korobeiniki.mp3
└── README.md
```

### 4.2 模块职责

| 模块 | 文件 | 职责 |
| --- | --- | --- |
| 游戏主逻辑 | `Core/Src/tetris.c` | 方块生成、移动、旋转、碰撞、消行、计分、界面绘制和菜单状态机 |
| GUI 参数 | `Core/Inc/tetris.h` | 小屏棋盘尺寸、布局位置、速度参数 |
| 旧 UI 备份 | `Core/Inc/tetris_ui_large_backup.h` | 保存旧大屏幕布局参数 |
| BGM 播放 | `Core/Src/audio_bgm.c` | ES8388 初始化、I2S DMA 播放、PCM 缓冲填充、音量控制 |
| BGM 数据 | `Core/Src/korobeiniki_pcm.c` | `Korobeiniki.mp3` 转换后的 PCM 数组 |
| EEPROM 设置 | `Core/Src/app_settings.c` | 最高分、音量、最近 10 次历史记录保存 |
| RTC 时间 | `Core/Src/app_rtc.c` | RTC 初始化、时间读取、时间设置 |
| LED 反馈 | `Core/Src/led_feedback.c` | LED0/LED1 状态机 |
| SPI FLASH | `Core/Src/spi_flash.c` | 软件 SPI、FLASH 读写、最近一局快照 |
| 程序入口 | `Core/Src/main.c` | HAL 初始化、外设初始化、应用模块初始化和主循环 |

### 4.3 主循环设计

`main.c` 中的主循环如下：

```c
while (1)
{
    Audio_BGM_Task();
    Led_Feedback_Task();
    tetris_loop();
}
```

设计原则：

- BGM 使用 DMA 循环播放，主循环中无需阻塞等待。
- LED 使用非阻塞状态机。
- 游戏逻辑使用 `HAL_GetTick()` 判断时间，实现按键消抖和重力下落。
- LCD 绘制只在必要时局部刷新，避免整屏频繁刷新导致闪烁。

## 5. 游戏逻辑设计

### 5.1 方块表示

每个方块使用 4x4 位图表示，一种方块对应 4 个旋转状态。

```c
const uint16_t shapes[7][4];
```

第 `j` 行、第 `i` 列对应：

```c
bit = j * 4 + i
```

这样可以用位运算快速判断某一格是否有方块。

### 5.2 棋盘表示

棋盘数组：

```c
static uint8_t board[BOARD_ROWS][BOARD_COLS];
```

含义：

- `0` 表示空格。
- `1..7` 表示方块颜色索引。

当前棋盘参数：

```c
#define BOARD_COLS   10
#define BOARD_ROWS   20
#define BLOCK_SIZE   12
#define BOARD_X_OFF  8
#define BOARD_Y_OFF  34
```

### 5.3 碰撞检测

移动和旋转前调用碰撞检测函数：

```c
static int check_hit(int x, int y, int rot);
```

检测内容：

- 是否超出左右边界。
- 是否超出底部。
- 是否与已固定方块重叠。

### 5.4 旋转修正

旋转时如果新形态右侧越界，会先向左修正；如果仍然碰撞，会尝试向右微调 1 到 2 格。这样可以改善靠墙旋转体验。

### 5.5 消行和计分

消行规则：

| 一次消除行数 | 得分 |
| --- | --- |
| 1 | 100 |
| 2 | 300 |
| 3 | 500 |
| 4 | 800 |

等级规则：

```c
level = lines_total / 10;
```

下落速度：

```c
fall_speed = FALL_SPEED_INIT - level * 30;
```

最低速度限制：

```c
#define FALL_SPEED_MIN 100
```

### 5.6 影子方块

影子方块通过不断向下试探当前位置直到碰撞，得到最终落点。绘制时只画矩形边框，不填充颜色，便于区分当前活动方块和落点提示。

## 6. GUI 设计说明

### 6.1 小屏适配

由于当前 LCD 尺寸较小，GUI 使用紧凑布局：

- 棋盘放在左侧，大小为 `10 * 12` x `20 * 12`。
- 右侧面板显示 NEXT、SCORE、LINES、LEVEL。
- 底部显示操作提示。
- 菜单和历史页均控制在 240x320 范围内。

### 6.2 方块视觉优化

方块绘制中加入：

- 主色填充。
- 左上白色高光。
- 右下暗色阴影。
- 内部小面积反光。

暗色通过 RGB565 分量缩放得到：

```c
static uint16_t dim_color(uint16_t color);
```

### 6.3 面板和按钮绘制

新增通用绘图函数：

```c
static void draw_panel_box(...);
static void draw_button(...);
```

用于统一绘制：

- 棋盘边框。
- 信息面板。
- 开始菜单按钮。
- Game Over 操作按钮。
- 历史记录信息框。

### 6.4 简单动效

消行时调用：

```c
static void flash_line(uint8_t row);
```

流程：

1. 满行闪白。
2. 恢复原方块颜色。
3. 闪黄提示。
4. 执行行下移。

每次闪烁延时较短，既有反馈，又不会明显影响游戏节奏。

## 7. BGM 实现说明

### 7.1 音频数据来源

项目使用 `Korobeiniki.mp3` 作为背景音乐。为了减少运行时解码复杂度，MP3 被预先转换为 PCM C 数组：

- `Core/Inc/korobeiniki_pcm.h`
- `Core/Src/korobeiniki_pcm.c`

当前 PCM 参数：

```c
#define KOROBEINIKI_PCM_SAMPLE_RATE 8000U
#define KOROBEINIKI_PCM_CHANNELS 1U
#define KOROBEINIKI_PCM_SAMPLE_COUNT 356572U
```

### 7.2 播放流程

1. 初始化 I2S 时钟。
2. 使用软件 I2C 配置 ES8388。
3. 初始化 I2S2 为主机发送模式。
4. 配置 DMA1 Stream4 为循环模式。
5. 启动 DMA 传输。
6. 在半传输和全传输回调中填充音频缓冲区。

### 7.3 音量控制

菜单音量为 0..10 档，实际 PCM 缩放如下：

```c
sample = sample * bgm_volume * AUDIO_VOLUME_LIMIT_PCT
         / (AUDIO_VOLUME_MAX * 100);
```

当前：

```c
#define AUDIO_VOLUME_MAX        10U
#define AUDIO_VOLUME_LIMIT_PCT  3U
```

因此最大输出约为原始音量的 3%。

## 8. EEPROM 数据格式

EEPROM 持久化结构位于 `app_settings.c`：

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t high_score;
    uint32_t high_lines;
    uint8_t bgm_volume;
    uint8_t history_count;
    uint8_t history_next;
    uint8_t reserved;
    app_game_history_t history[APP_SETTINGS_HISTORY_CAPACITY];
    uint16_t checksum;
} settings_record_t;
```

历史记录结构：

```c
typedef struct {
    uint32_t score;
    uint32_t lines;
    uint32_t duration_seconds;
    app_datetime_t started_at;
} app_game_history_t;
```

容量说明：

- EEPROM 总容量为 256 字节。
- 当前最多保存 10 条历史记录。
- 使用 `_Static_assert` 确保结构体不会超过 EEPROM 容量。

历史记录使用环形缓冲区：

- `history_next` 指向下一次写入位置。
- 最新记录通过 `history_next` 向前倒推读取。
- 记录满后，新记录覆盖最旧记录。

## 9. RTC 设计说明

### 9.1 初始化流程

RTC 初始化函数：

```c
void App_RTC_Init(void);
```

流程：

1. 使能 PWR 时钟。
2. 允许访问备份域。
3. 尝试启用 LSE。
4. LSE 失败则复位备份域并启用 LSI。
5. 初始化 RTC。
6. 检查备份寄存器标记。
7. 若标记不存在，使用编译时间写入 RTC。

### 9.2 默认时间

默认时间来自编译宏：

```c
__DATE__
__TIME__
```

如果编译年份早于 2026，则强制修正为 2026，避免异常年份进入历史记录。

### 9.3 时间记录

开始新游戏时读取 RTC：

```c
App_RTC_GetDateTime(&game_start_time);
```

Game Over 时写入 EEPROM 历史：

```c
App_Settings_AddGameResult(score, lines_total, duration_seconds, &game_start_time);
```

## 10. 操作说明

### 10.1 开始菜单

| 操作 | 功能 |
| --- | --- |
| KEY0 | 开始游戏 |
| KEY1 | BGM 音量减 |
| KEY2 | BGM 音量加 |
| KEY_UP | 打开或关闭 EEPROM 历史记录页 |

### 10.2 历史记录页

| 操作 | 功能 |
| --- | --- |
| KEY1 | 上一页 |
| KEY2 | 下一页 |
| KEY_UP | 返回开始菜单 |

### 10.3 游戏中

屏幕底部显示当前按键说明。当前实现支持：

- 左移。
- 右移。
- 旋转。
- 硬降。
- 红外遥控按键控制。

### 10.4 Game Over 页面

| 操作 | 功能 |
| --- | --- |
| KEY0 | 直接重新开始 |
| KEY_UP | 返回开始菜单 |

返回开始菜单后可以继续查看 EEPROM 历史记录或调整 BGM 音量。

## 11. 构建和烧录

### 11.1 构建环境

项目使用 CMake 构建，目标平台为 STM32F407。

常用构建命令：

```powershell
cmake --build build/Debug --
```

### 11.2 当前构建结果

最近一次构建通过，资源占用如下：

```text
RAM:   5024 B / 128 KB   3.83%
FLASH: 775464 B / 1 MB   73.95%
```

生成文件：

- `build/Debug/STM32F407_Tetris.elf`
- `build/Debug/STM32F407_Tetris.hex`

可使用 ST-Link、STM32CubeProgrammer 或对应 IDE 将 HEX/ELF 烧录到开发板。

## 12. 测试情况

已验证内容：

- 工程可以完成编译和链接。
- GUI 已适配 240x320 竖屏显示区域。
- 开始菜单、历史页、游戏界面和 Game Over 页面均已重新布局。
- BGM 可通过 I2S DMA 播放。
- 音量可在菜单调整并保存到 EEPROM。
- Game Over 后可以返回开始菜单。
- EEPROM 可保存最高分、音量和最近历史记录。
- RTC 时间可写入历史记录。
- LED0/LED1 可根据游戏状态反馈。
- SPI FLASH 可保存最近一局快照。

建议上板重点检查：

- KEY0/KEY1/KEY2/KEY_UP 的实际方向是否与屏幕提示完全一致。
- 板载 LSE 是否正常起振；如果时间不准，需要确认是否回退到了 LSI。
- BGM 音量是否仍然偏大，如偏大可继续降低 `AUDIO_VOLUME_LIMIT_PCT` 或 ES8388 模拟音量。
- 消行动画延时是否符合手感。

## 13. 已知限制

- EEPROM 容量只有 256 字节，无法无限保存每一局历史，因此当前仅保存最近 10 局。
- RTC 首次默认时间来自固件编译时间，如果需要真实当前时间，需要增加手动校时功能。
- BGM 目前以 PCM C 数组存放在片内 Flash，占用较多 Flash 空间。
- SPI FLASH 当前只保存最近一局快照，尚未用于 BGM 文件流式播放。
- GUI 使用 LCD 基础绘图函数实现，没有引入复杂图形库，因此动画以短促反馈为主。
- 部分按键宏名称和实际游戏动作存在历史适配关系，维护时应以 `tetris.c` 的按键处理逻辑为准。

## 14. 后续改进方向

可以继续扩展以下功能：

1. 增加 RTC 手动校时页面。
2. 使用 SPI FLASH 或 U 盘加载 BGM，减少片内 Flash 占用。
3. 将 EEPROM 历史记录迁移到 SPI FLASH，保存更多局数。
4. 增加暂停功能。
5. 增加难度选择。
6. 增加静音开关。
7. 增加开机自检页面，显示 LCD、EEPROM、SPI FLASH、RTC、ES8388 状态。
8. 优化红外遥控按键映射，使其与板载按键完全一致。
9. 增加更丰富的消行动效和等级提升提示。
10. 增加最高分清除或恢复默认设置功能。

## 15. 关键文件索引

| 文件 | 说明 |
| --- | --- |
| `Core/Src/main.c` | 程序入口和模块初始化 |
| `Core/Src/tetris.c` | 游戏主逻辑、GUI、菜单、历史页 |
| `Core/Inc/tetris.h` | 棋盘和 UI 布局参数 |
| `Core/Inc/tetris_ui_large_backup.h` | 旧大屏 UI 参数备份 |
| `Core/Src/audio_bgm.c` | BGM 播放和 ES8388/I2S DMA |
| `Core/Src/korobeiniki_pcm.c` | BGM PCM 数据 |
| `Core/Src/app_settings.c` | EEPROM 设置和历史记录 |
| `Core/Src/app_rtc.c` | RTC 初始化和时间读写 |
| `Core/Src/led_feedback.c` | LED0/LED1 状态反馈 |
| `Core/Src/spi_flash.c` | SPI FLASH 读写和最近记录 |
| `Core/Src/lcd.c` | LCD 基础驱动 |
| `Core/Src/stm32f4xx_it.c` | 中断和红外解码相关逻辑 |
