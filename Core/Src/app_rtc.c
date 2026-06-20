#include "app_rtc.h"

#include <string.h>

#define RTC_BACKUP_MARKER      0xA55AU
#define RTC_DEFAULT_YEAR       2026U

RTC_HandleTypeDef hrtc;

static uint8_t rtc_running;

/* 将 __DATE__ 的英文月份缩写转换为 1..12。 */
static uint8_t month_from_build(const char *mon)
{
    static const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

    for (uint8_t i = 0; i < 12; i++) {
        if (memcmp(mon, &names[i * 3], 3) == 0) {
            return (uint8_t)(i + 1U);
        }
    }
    return 1;
}

static uint8_t dec2(const char *s)
{
    return (uint8_t)((s[0] - '0') * 10 + (s[1] - '0'));
}

static void build_datetime(app_datetime_t *dt)
{
    const char *date = __DATE__;
    const char *time = __TIME__;

    /* 没有用户手动校时时，使用编译时间作为首次启动的默认 RTC 时间。 */
    dt->year = (uint16_t)((date[7] - '0') * 1000 + (date[8] - '0') * 100 +
                          (date[9] - '0') * 10 + (date[10] - '0'));
    dt->month = month_from_build(date);
    dt->day = (uint8_t)(((date[4] == ' ') ? 0 : (date[4] - '0') * 10) + (date[5] - '0'));
    dt->hour = dec2(time);
    dt->minute = dec2(time + 3);
    dt->second = dec2(time + 6);

    if (dt->year < RTC_DEFAULT_YEAR) {
        dt->year = RTC_DEFAULT_YEAR;
    }
}

static uint8_t rtc_clock_config(uint8_t use_lse)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_PeriphCLKInitTypeDef periph = {0};

    HAL_PWR_EnableBkUpAccess();

    /* 优先选择 LSE；如果板上晶振未焊接或起振失败，调用方会回退到 LSI。 */
    if (use_lse) {
        osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
        osc.LSEState = RCC_LSE_ON;
        if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
            return 0;
        }
        periph.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    } else {
        osc.OscillatorType = RCC_OSCILLATORTYPE_LSI;
        osc.LSIState = RCC_LSI_ON;
        if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
            return 0;
        }
        periph.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
    }

    periph.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    if (HAL_RCCEx_PeriphCLKConfig(&periph) != HAL_OK) {
        return 0;
    }

    __HAL_RCC_RTC_ENABLE();
    return 1;
}

void App_RTC_Init(void)
{
    app_datetime_t dt;
    uint8_t marker_valid;

    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    /* LSE 失败时复位备份域并改用 LSI，保证历史记录至少有可用时间。 */
    if (!rtc_clock_config(1)) {
        __HAL_RCC_BACKUPRESET_FORCE();
        __HAL_RCC_BACKUPRESET_RELEASE();
        (void)rtc_clock_config(0);
    }

    hrtc.Instance = RTC;
    hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
    hrtc.Init.AsynchPrediv = 127;
    hrtc.Init.SynchPrediv = 255;
    hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
    hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;

    if (HAL_RTC_Init(&hrtc) != HAL_OK) {
        rtc_running = 0;
        return;
    }

    rtc_running = 1;
    marker_valid = (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) == RTC_BACKUP_MARKER);
    if (!marker_valid) {
        /* 备份寄存器没有标记，说明 RTC 时间尚未初始化。 */
        build_datetime(&dt);
        (void)App_RTC_SetDateTime(&dt);
        HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, RTC_BACKUP_MARKER);
    }
}

uint8_t App_RTC_IsRunning(void)
{
    return rtc_running;
}

void App_RTC_GetDateTime(app_datetime_t *dt)
{
    RTC_DateTypeDef date;
    RTC_TimeTypeDef time;

    if (dt == 0) {
        return;
    }

    if (!rtc_running ||
        HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN) != HAL_OK ||
        HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN) != HAL_OK) {
        /* RTC 不可用时返回编译时间，避免历史记录出现全 0。 */
        build_datetime(dt);
        return;
    }

    dt->year = (uint16_t)(2000U + date.Year);
    dt->month = date.Month;
    dt->day = date.Date;
    dt->hour = time.Hours;
    dt->minute = time.Minutes;
    dt->second = time.Seconds;
}

uint8_t App_RTC_SetDateTime(const app_datetime_t *dt)
{
    RTC_DateTypeDef date = {0};
    RTC_TimeTypeDef time = {0};

    if (dt == 0 || !rtc_running || dt->year < 2000U || dt->year > 2099U ||
        dt->month == 0U || dt->month > 12U || dt->day == 0U || dt->day > 31U ||
        dt->hour > 23U || dt->minute > 59U || dt->second > 59U) {
        return 0;
    }

    /* HAL RTC 年份字段只保存 2000 年后的两位偏移。 */
    date.Year = (uint8_t)(dt->year - 2000U);
    date.Month = dt->month;
    date.Date = dt->day;
    date.WeekDay = RTC_WEEKDAY_MONDAY;
    time.Hours = dt->hour;
    time.Minutes = dt->minute;
    time.Seconds = dt->second;
    time.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    time.StoreOperation = RTC_STOREOPERATION_RESET;

    if (HAL_RTC_SetTime(&hrtc, &time, RTC_FORMAT_BIN) != HAL_OK) {
        return 0;
    }
    if (HAL_RTC_SetDate(&hrtc, &date, RTC_FORMAT_BIN) != HAL_OK) {
        return 0;
    }

    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, RTC_BACKUP_MARKER);
    return 1;
}
