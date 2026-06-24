/**
 * @file app_settings.h
 * @brief EEPROM 设置与历史记录接口。
 *
 * 本模块负责把最高分、最高行数、BGM 音量以及最近游戏记录保存到
 * 板载 EEPROM。由于 EEPROM 容量有限，历史记录采用固定容量环形表。
 */
#ifndef __APP_SETTINGS_H
#define __APP_SETTINGS_H

#include "main.h"
#include "app_rtc.h"

/* 板载 EEPROM 只有 256 字节，因此历史记录保存为紧凑的最近记录列表。 */
#define APP_SETTINGS_HISTORY_CAPACITY 10U

/* 单次游戏记录。started_at 来自开局时读取到的 RTC 时间。 */
typedef struct {
    uint32_t score;              /* 单局最终得分。 */
    uint32_t lines;              /* 单局累计消除行数。 */
    uint32_t duration_seconds;   /* 单局持续时间，单位秒，由 HAL tick 计算。 */
    app_datetime_t started_at;   /* 单局开始时间，由 RTC 在开局时读取。 */
} app_game_history_t;

/* EEPROM 设置和分数记录在内存中的运行时副本。 */
typedef struct {
    uint32_t high_score;  /* EEPROM 中保存的历史最高分。 */
    uint32_t high_lines;  /* EEPROM 中保存的最高消除行数。 */
    uint8_t bgm_volume;   /* 用户 BGM 音量档位，范围 0..10。 */
    uint8_t history_count;/* 当前已有历史记录数量，不超过 APP_SETTINGS_HISTORY_CAPACITY。 */
    uint8_t history_next; /* 下一条历史记录写入位置，用于环形覆盖最旧记录。 */
    app_game_history_t history[APP_SETTINGS_HISTORY_CAPACITY]; /* 最近游戏记录数组。 */
} app_settings_t;

/* 初始化 EEPROM 软件 I2C，并读取或创建默认设置记录。 */
void App_Settings_Init(void);
/* 获取当前内存中的设置副本，调用者只读使用，不应直接修改。 */
const app_settings_t *App_Settings_Get(void);
/* 设置并保存 BGM 音量，输入大于 10 时会自动钳位。 */
void App_Settings_SetVolume(uint8_t volume);
/* 根据本局结果刷新最高分/最高行数，发生更新时返回 1。 */
uint8_t App_Settings_UpdateHighScore(uint32_t score, uint32_t lines);
/* 添加一条游戏历史，并同步更新最高分和 EEPROM 内容。 */
void App_Settings_AddGameResult(uint32_t score, uint32_t lines, uint32_t duration_seconds,
                                const app_datetime_t *started_at);
/* 按“从新到旧”的顺序读取历史记录，latest_index=0 表示最近一局。 */
uint8_t App_Settings_GetHistory(uint8_t latest_index, app_game_history_t *record);

#endif /* __APP_SETTINGS_H */
