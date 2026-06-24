/**
 * @file app_rtc.h
 * @brief 应用层 RTC 时间接口，负责为游戏历史记录提供年月日时分秒。
 *
 * 本模块屏蔽 HAL RTC 的年份偏移、LSE/LSI 时钟选择等细节，
 * 游戏和 EEPROM 模块只需要使用 app_datetime_t 这个普通日期结构。
 */
#ifndef __APP_RTC_H
#define __APP_RTC_H

#include "main.h"

/* EEPROM 历史记录使用的日期时间格式。
 * year 保存完整年份，例如 2026；HAL RTC 内部使用 00..99。
 */
typedef struct {
    uint16_t year;   /* 完整年份，例如 2026，用于界面显示和历史记录保存。 */
    uint8_t month;   /* 月份，范围 1..12。 */
    uint8_t day;     /* 日期，范围 1..31，当前只做基础范围检查。 */
    uint8_t hour;    /* 小时，24 小时制，范围 0..23。 */
    uint8_t minute;  /* 分钟，范围 0..59。 */
    uint8_t second;  /* 秒，范围 0..59。 */
} app_datetime_t;

/* RTC 优先使用外部低速晶振 LSE；如果 LSE 启动失败则回退到 LSI。
 * 首次上电或备份域复位后，会使用固件编译时间作为初始时间。
 */
void App_RTC_Init(void);
/* 返回 RTC 当前是否成功初始化，1 表示可用，0 表示 HAL RTC 初始化失败。 */
uint8_t App_RTC_IsRunning(void);
/* 读取当前日期时间；RTC 不可用时返回固件编译时间作为兜底值。 */
void App_RTC_GetDateTime(app_datetime_t *dt);
/* 设置 RTC 日期时间；参数非法或 RTC 未运行时返回 0，成功返回 1。 */
uint8_t App_RTC_SetDateTime(const app_datetime_t *dt);

#endif /* __APP_RTC_H */
