#ifndef __APP_SETTINGS_H
#define __APP_SETTINGS_H

#include "main.h"
#include "app_rtc.h"

/* 板载 EEPROM 只有 256 字节，因此历史记录保存为紧凑的最近记录列表。 */
#define APP_SETTINGS_HISTORY_CAPACITY 10U

/* 单次游戏记录。started_at 来自开局时读取到的 RTC 时间。 */
typedef struct {
    uint32_t score;
    uint32_t lines;
    uint32_t duration_seconds;
    app_datetime_t started_at;
} app_game_history_t;

/* EEPROM 设置和分数记录在内存中的运行时副本。 */
typedef struct {
    uint32_t high_score;
    uint32_t high_lines;
    uint8_t bgm_volume;
    uint8_t history_count;
    uint8_t history_next;
    app_game_history_t history[APP_SETTINGS_HISTORY_CAPACITY];
} app_settings_t;

void App_Settings_Init(void);
const app_settings_t *App_Settings_Get(void);
void App_Settings_SetVolume(uint8_t volume);
uint8_t App_Settings_UpdateHighScore(uint32_t score, uint32_t lines);
void App_Settings_AddGameResult(uint32_t score, uint32_t lines, uint32_t duration_seconds,
                                const app_datetime_t *started_at);
uint8_t App_Settings_GetHistory(uint8_t latest_index, app_game_history_t *record);

#endif /* __APP_SETTINGS_H */
