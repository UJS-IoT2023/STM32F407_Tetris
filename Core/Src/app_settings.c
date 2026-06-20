#include "app_settings.h"

#include <stddef.h>
#include <string.h>

#define EEPROM_I2C_ADDR         0xA0U
#define EEPROM_SETTINGS_ADDR    0U
#define EEPROM_PAGE_SIZE        8U
#define EEPROM_SIZE_BYTES       256U

#define SETTINGS_MAGIC          0x53525454UL
#define SETTINGS_VERSION        3U
#define SETTINGS_DEFAULT_VOLUME 5U

#define EE_SCL_Pin              GPIO_PIN_8
#define EE_SCL_GPIO_Port        GPIOB
#define EE_SDA_Pin              GPIO_PIN_9
#define EE_SDA_GPIO_Port        GPIOB

/* EEPROM 中的实际持久化格式。magic/version/size 用于识别格式升级，
 * checksum 用于过滤掉异常掉电或总线错误导致的脏数据。
 */
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

_Static_assert(sizeof(settings_record_t) <= EEPROM_SIZE_BYTES,
               "settings_record_t must fit into EEPROM");

static app_settings_t settings;

/* PB8/PB9 与 ES8388 共用，这里用软件 I2C 访问 AT24C02 类 EEPROM。 */
static void ee_delay(void)
{
    for (volatile uint32_t i = 0; i < 60; i++) {
        __NOP();
    }
}

static void ee_sda_out(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = EE_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(EE_SDA_GPIO_Port, &GPIO_InitStruct);
}

static void ee_sda_in(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = EE_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(EE_SDA_GPIO_Port, &GPIO_InitStruct);
}

static void ee_i2c_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitStruct.Pin = EE_SCL_Pin | EE_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    HAL_GPIO_WritePin(EE_SCL_GPIO_Port, EE_SCL_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(EE_SDA_GPIO_Port, EE_SDA_Pin, GPIO_PIN_SET);
}

static void ee_start(void)
{
    ee_sda_out();
    HAL_GPIO_WritePin(EE_SDA_GPIO_Port, EE_SDA_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(EE_SCL_GPIO_Port, EE_SCL_Pin, GPIO_PIN_SET);
    ee_delay();
    HAL_GPIO_WritePin(EE_SDA_GPIO_Port, EE_SDA_Pin, GPIO_PIN_RESET);
    ee_delay();
    HAL_GPIO_WritePin(EE_SCL_GPIO_Port, EE_SCL_Pin, GPIO_PIN_RESET);
}

static void ee_stop(void)
{
    ee_sda_out();
    HAL_GPIO_WritePin(EE_SCL_GPIO_Port, EE_SCL_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(EE_SDA_GPIO_Port, EE_SDA_Pin, GPIO_PIN_RESET);
    ee_delay();
    HAL_GPIO_WritePin(EE_SCL_GPIO_Port, EE_SCL_Pin, GPIO_PIN_SET);
    ee_delay();
    HAL_GPIO_WritePin(EE_SDA_GPIO_Port, EE_SDA_Pin, GPIO_PIN_SET);
    ee_delay();
}

static uint8_t ee_wait_ack(void)
{
    uint8_t ack;

    ee_sda_in();
    HAL_GPIO_WritePin(EE_SCL_GPIO_Port, EE_SCL_Pin, GPIO_PIN_SET);
    ee_delay();
    ack = (HAL_GPIO_ReadPin(EE_SDA_GPIO_Port, EE_SDA_Pin) == GPIO_PIN_RESET);
    HAL_GPIO_WritePin(EE_SCL_GPIO_Port, EE_SCL_Pin, GPIO_PIN_RESET);
    ee_sda_out();
    return ack;
}

static uint8_t ee_send_byte(uint8_t data)
{
    ee_sda_out();
    for (uint8_t i = 0; i < 8; i++) {
        HAL_GPIO_WritePin(EE_SDA_GPIO_Port, EE_SDA_Pin,
                          (data & 0x80U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        data <<= 1;
        ee_delay();
        HAL_GPIO_WritePin(EE_SCL_GPIO_Port, EE_SCL_Pin, GPIO_PIN_SET);
        ee_delay();
        HAL_GPIO_WritePin(EE_SCL_GPIO_Port, EE_SCL_Pin, GPIO_PIN_RESET);
    }
    return ee_wait_ack();
}

static uint8_t ee_read_byte(uint8_t ack)
{
    uint8_t data = 0;

    ee_sda_in();
    for (uint8_t i = 0; i < 8; i++) {
        data <<= 1;
        HAL_GPIO_WritePin(EE_SCL_GPIO_Port, EE_SCL_Pin, GPIO_PIN_SET);
        ee_delay();
        if (HAL_GPIO_ReadPin(EE_SDA_GPIO_Port, EE_SDA_Pin) == GPIO_PIN_SET) {
            data |= 1U;
        }
        HAL_GPIO_WritePin(EE_SCL_GPIO_Port, EE_SCL_Pin, GPIO_PIN_RESET);
        ee_delay();
    }

    ee_sda_out();
    HAL_GPIO_WritePin(EE_SDA_GPIO_Port, EE_SDA_Pin, ack ? GPIO_PIN_RESET : GPIO_PIN_SET);
    ee_delay();
    HAL_GPIO_WritePin(EE_SCL_GPIO_Port, EE_SCL_Pin, GPIO_PIN_SET);
    ee_delay();
    HAL_GPIO_WritePin(EE_SCL_GPIO_Port, EE_SCL_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(EE_SDA_GPIO_Port, EE_SDA_Pin, GPIO_PIN_SET);

    return data;
}

static uint8_t eeprom_read(uint8_t addr, uint8_t *buf, uint16_t len)
{
    if ((uint16_t)addr + len > EEPROM_SIZE_BYTES) {
        return 0;
    }

    ee_start();
    if (!ee_send_byte(EEPROM_I2C_ADDR | 0U) || !ee_send_byte(addr)) {
        ee_stop();
        return 0;
    }
    ee_start();
    if (!ee_send_byte(EEPROM_I2C_ADDR | 1U)) {
        ee_stop();
        return 0;
    }

    for (uint16_t i = 0; i < len; i++) {
        buf[i] = ee_read_byte(i + 1U < len);
    }
    ee_stop();
    return 1;
}

static uint8_t eeprom_write_page(uint8_t addr, const uint8_t *buf, uint8_t len)
{
    ee_start();
    if (!ee_send_byte(EEPROM_I2C_ADDR | 0U) || !ee_send_byte(addr)) {
        ee_stop();
        return 0;
    }

    for (uint8_t i = 0; i < len; i++) {
        if (!ee_send_byte(buf[i])) {
            ee_stop();
            return 0;
        }
    }
    ee_stop();
    HAL_Delay(6);
    return 1;
}

static uint8_t eeprom_write(uint8_t addr, const uint8_t *buf, uint16_t len)
{
    /* EEPROM 页写入不能跨 8 字节页边界，按页切分后再写。 */
    while (len != 0U) {
        uint8_t page_space = EEPROM_PAGE_SIZE - (addr % EEPROM_PAGE_SIZE);
        uint8_t chunk = (len < page_space) ? (uint8_t)len : page_space;
        if (!eeprom_write_page(addr, buf, chunk)) {
            return 0;
        }
        addr = (uint8_t)(addr + chunk);
        buf += chunk;
        len = (uint16_t)(len - chunk);
    }
    return 1;
}

static uint16_t checksum16(const uint8_t *buf, uint16_t len)
{
    uint16_t sum = 0xA55AU;
    for (uint16_t i = 0; i < len; i++) {
        sum = (uint16_t)((sum << 5) | (sum >> 11));
        sum ^= buf[i];
    }
    return sum;
}

static settings_record_t make_record_from_settings(void)
{
    settings_record_t rec = {0};

    /* 每次保存都从内存状态重新打包，避免结构体未使用字节影响校验。 */
    rec.magic = SETTINGS_MAGIC;
    rec.version = SETTINGS_VERSION;
    rec.size = sizeof(rec);
    rec.high_score = settings.high_score;
    rec.high_lines = settings.high_lines;
    rec.bgm_volume = settings.bgm_volume;
    rec.history_count = settings.history_count;
    rec.history_next = settings.history_next;
    memcpy(rec.history, settings.history, sizeof(rec.history));
    rec.checksum = checksum16((const uint8_t *)&rec, offsetof(settings_record_t, checksum));

    return rec;
}

static uint8_t record_is_valid(const settings_record_t *rec)
{
    uint16_t crc = checksum16((const uint8_t *)rec, offsetof(settings_record_t, checksum));

    /* 版本或大小变化时丢弃旧记录，使用默认值重新初始化 EEPROM。 */
    return rec->magic == SETTINGS_MAGIC &&
           rec->version == SETTINGS_VERSION &&
           rec->size == sizeof(*rec) &&
           rec->checksum == crc &&
           rec->bgm_volume <= 10U &&
           rec->history_count <= APP_SETTINGS_HISTORY_CAPACITY &&
           rec->history_next < APP_SETTINGS_HISTORY_CAPACITY;
}

static void save_settings(void)
{
    settings_record_t rec = make_record_from_settings();
    settings_record_t verify;

    /* 写后读回校验一次；如果失败则重写，提高掉电边缘情况下的可靠性。 */
    (void)eeprom_write(EEPROM_SETTINGS_ADDR, (const uint8_t *)&rec, sizeof(rec));
    if (!eeprom_read(EEPROM_SETTINGS_ADDR, (uint8_t *)&verify, sizeof(verify)) ||
        !record_is_valid(&verify)) {
        (void)eeprom_write(EEPROM_SETTINGS_ADDR, (const uint8_t *)&rec, sizeof(rec));
    }
}

void App_Settings_Init(void)
{
    settings_record_t rec;

    ee_i2c_init();
    /* 先准备默认值，读取失败或格式不匹配时直接落盘为新记录。 */
    settings.high_score = 0;
    settings.high_lines = 0;
    settings.bgm_volume = SETTINGS_DEFAULT_VOLUME;
    settings.history_count = 0;
    settings.history_next = 0;
    memset(settings.history, 0, sizeof(settings.history));

    if (!eeprom_read(EEPROM_SETTINGS_ADDR, (uint8_t *)&rec, sizeof(rec))) {
        save_settings();
        return;
    }

    if (!record_is_valid(&rec)) {
        save_settings();
        return;
    }

    settings.high_score = rec.high_score;
    settings.high_lines = rec.high_lines;
    settings.bgm_volume = rec.bgm_volume;
    settings.history_count = rec.history_count;
    settings.history_next = rec.history_next;
    memcpy(settings.history, rec.history, sizeof(settings.history));
}

const app_settings_t *App_Settings_Get(void)
{
    return &settings;
}

void App_Settings_SetVolume(uint8_t volume)
{
    /* 菜单音量固定为 0..10 档，实际输出还会在 audio_bgm 中再限幅。 */
    if (volume > 10U) {
        volume = 10U;
    }

    if (settings.bgm_volume != volume) {
        settings.bgm_volume = volume;
        save_settings();
    }
}

uint8_t App_Settings_UpdateHighScore(uint32_t score, uint32_t lines)
{
    if (score > settings.high_score ||
        (score == settings.high_score && lines > settings.high_lines)) {
        settings.high_score = score;
        settings.high_lines = lines;
        save_settings();
        return 1;
    }

    return 0;
}

void App_Settings_AddGameResult(uint32_t score, uint32_t lines, uint32_t duration_seconds,
                                const app_datetime_t *started_at)
{
    app_game_history_t *record = &settings.history[settings.history_next];

    /* history 是环形缓冲区：最新记录覆盖最旧记录，节省 EEPROM 空间。 */
    record->score = score;
    record->lines = lines;
    record->duration_seconds = duration_seconds;
    if (started_at != 0) {
        record->started_at = *started_at;
    } else {
        memset(&record->started_at, 0, sizeof(record->started_at));
    }

    settings.history_next++;
    if (settings.history_next >= APP_SETTINGS_HISTORY_CAPACITY) {
        settings.history_next = 0;
    }
    if (settings.history_count < APP_SETTINGS_HISTORY_CAPACITY) {
        settings.history_count++;
    }

    (void)App_Settings_UpdateHighScore(score, lines);
    save_settings();
}

uint8_t App_Settings_GetHistory(uint8_t latest_index, app_game_history_t *record)
{
    uint8_t stored_index;

    if (record == 0 || latest_index >= settings.history_count) {
        return 0;
    }

    /* latest_index=0 表示最近一局，需要从 history_next 向前倒推。 */
    stored_index = (uint8_t)((settings.history_next + APP_SETTINGS_HISTORY_CAPACITY - 1U - latest_index) %
                             APP_SETTINGS_HISTORY_CAPACITY);
    *record = settings.history[stored_index];
    return 1;
}
