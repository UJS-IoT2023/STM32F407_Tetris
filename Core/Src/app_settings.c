/**
 * @file app_settings.c
 * @brief EEPROM 中保存用户设置、最高分和最近游戏历史的实现。
 *
 * 模块使用 PB8/PB9 软件 I2C 访问 256 字节 EEPROM。保存格式包含
 * magic、version、size 和 checksum，便于固件升级后识别旧格式并恢复默认值。
 */
#include "app_settings.h"

#include <stddef.h>
#include <string.h>

#define EEPROM_I2C_ADDR         0xA0U /* AT24C02 类 EEPROM 的 8 位器件地址。 */
#define EEPROM_SETTINGS_ADDR    0U    /* 设置记录从 EEPROM 第 0 字节开始保存。 */
#define EEPROM_PAGE_SIZE        8U    /* EEPROM 单页写入大小，跨页需拆分。 */
#define EEPROM_SIZE_BYTES       256U  /* 板载 EEPROM 总容量。 */

#define SETTINGS_MAGIC          0x53525454UL /* 设置记录有效性标记，ASCII 近似 SRTT。 */
#define SETTINGS_VERSION        3U          /* EEPROM 记录格式版本，结构变化时递增。 */
#define SETTINGS_DEFAULT_VOLUME 5U          /* EEPROM 无效时使用的默认 BGM 音量。 */

#define EE_SCL_Pin              GPIO_PIN_8 /* EEPROM 软件 I2C 时钟引脚。 */
#define EE_SCL_GPIO_Port        GPIOB      /* EEPROM SCL 所在端口。 */
#define EE_SDA_Pin              GPIO_PIN_9 /* EEPROM 软件 I2C 数据引脚。 */
#define EE_SDA_GPIO_Port        GPIOB      /* EEPROM SDA 所在端口。 */

/* EEPROM 中的实际持久化格式。magic/version/size 用于识别格式升级，
 * checksum 用于过滤掉异常掉电或总线错误导致的脏数据。
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;       /* 固定标记，用来判断 EEPROM 是否已写入有效设置。 */
    uint16_t version;     /* 结构版本，和 SETTINGS_VERSION 不一致时重建记录。 */
    uint16_t size;        /* 当前结构体大小，防止编译配置变化导致误读。 */
    uint32_t high_score;  /* 历史最高分。 */
    uint32_t high_lines;  /* 历史最高消除行数。 */
    uint8_t bgm_volume;   /* BGM 音量档位，范围 0..10。 */
    uint8_t history_count;/* 当前有效历史条数。 */
    uint8_t history_next; /* 下一次写入 history 的下标。 */
    uint8_t reserved;     /* 预留字节，保持结构对齐和后续扩展空间。 */
    app_game_history_t history[APP_SETTINGS_HISTORY_CAPACITY]; /* 最近游戏记录。 */
    uint16_t checksum;    /* 对 checksum 前所有字节计算得到的校验值。 */
} settings_record_t;

_Static_assert(sizeof(settings_record_t) <= EEPROM_SIZE_BYTES,
               "settings_record_t must fit into EEPROM");

static app_settings_t settings; /* EEPROM 内容在 RAM 中的工作副本。 */

/* PB8/PB9 与 ES8388 共用，这里用软件 I2C 访问 AT24C02 类 EEPROM。 */
/* 软件 I2C 半周期短延时，用 NOP 保持时序简单稳定。 */
static void ee_delay(void)
{
    for (volatile uint32_t i = 0; i < 60; i++) {
        __NOP();
    }
}

/* 将 SDA 配置为开漏输出，用于主机发送数据或 ACK。 */
static void ee_sda_out(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = EE_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(EE_SDA_GPIO_Port, &GPIO_InitStruct);
}

/* 将 SDA 配置为输入，用于读取 EEPROM ACK 或数据位。 */
static void ee_sda_in(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = EE_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(EE_SDA_GPIO_Port, &GPIO_InitStruct);
}

/* 初始化 EEPROM 软件 I2C 引脚，并释放总线到空闲高电平。 */
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

/* 产生 I2C 起始条件：SCL 高电平期间 SDA 从高拉低。 */
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

/* 产生 I2C 停止条件：SCL 高电平期间 SDA 从低释放为高。 */
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

/* 读取从机 ACK，应答为 SDA 低电平时返回 1。 */
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

/* 发送一个字节，高位先发，并等待 EEPROM 应答。 */
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

/* 读取一个字节；ack=1 继续读取，ack=0 表示最后一个字节。 */
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

/* 从 EEPROM 指定地址读取 len 字节，越界或无应答返回 0。 */
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

/* 写入 EEPROM 单页内数据，调用者需保证不跨页。 */
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

/* 写入任意长度数据，内部按 EEPROM 页边界拆分。 */
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

/* 计算设置记录的 16 位滚动校验值。 */
static uint16_t checksum16(const uint8_t *buf, uint16_t len)
{
    uint16_t sum = 0xA55AU;
    for (uint16_t i = 0; i < len; i++) {
        sum = (uint16_t)((sum << 5) | (sum >> 11));
        sum ^= buf[i];
    }
    return sum;
}

/* 将 RAM 中的 settings 打包为 EEPROM 持久化结构。 */
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

/* 检查 EEPROM 记录的标记、版本、大小、校验和字段范围。 */
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

/* 保存当前 settings 到 EEPROM，并进行一次写后读回验证。 */
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

/* 初始化设置模块，读取 EEPROM；失败或无效时写入默认记录。 */
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

/* 返回当前设置的只读指针，供菜单和游戏结算查询。 */
const app_settings_t *App_Settings_Get(void)
{
    return &settings;
}

/* 更新 BGM 音量并立即写入 EEPROM。 */
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

/* 判断本局成绩是否刷新最高分或最高行数，并在刷新时保存。 */
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

/* 添加一条游戏历史记录，并同步维护最高分。 */
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

/* 按从新到旧读取历史记录，latest_index=0 代表最近一局。 */
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
