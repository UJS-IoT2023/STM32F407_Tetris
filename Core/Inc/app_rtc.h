#ifndef __APP_RTC_H
#define __APP_RTC_H

#include "main.h"

/* EEPROM 历史记录使用的日期时间格式。
 * year 保存完整年份，例如 2026；HAL RTC 内部使用 00..99。
 */
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} app_datetime_t;

/* RTC 优先使用外部低速晶振 LSE；如果 LSE 启动失败则回退到 LSI。
 * 首次上电或备份域复位后，会使用固件编译时间作为初始时间。
 */
void App_RTC_Init(void);
uint8_t App_RTC_IsRunning(void);
void App_RTC_GetDateTime(app_datetime_t *dt);
uint8_t App_RTC_SetDateTime(const app_datetime_t *dt);

#endif /* __APP_RTC_H */
