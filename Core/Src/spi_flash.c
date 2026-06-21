/**
 * @file spi_flash.c
 * @brief 外部 SPI FLASH 软件 SPI 读写与最近游戏记录保存。
 *
 * 当前实现只依赖 GPIO 模拟 SPI 时序，适合在 SPI2 已被 I2S 音频占用时
 * 继续使用板载 SPI FLASH。保存记录时附加 magic 和 checksum 以判断有效性。
 */
#include "spi_flash.h"

#include <stddef.h>

#define FLASH_CS_Pin        GPIO_PIN_14 /* SPI FLASH 片选信号。 */
#define FLASH_CS_Port       GPIOB       /* 片选 GPIO 端口。 */
#define FLASH_SCK_Pin       GPIO_PIN_3  /* 软件 SPI 时钟信号。 */
#define FLASH_SCK_Port      GPIOB       /* 时钟 GPIO 端口。 */
#define FLASH_MISO_Pin      GPIO_PIN_4  /* 软件 SPI 主入从出信号。 */
#define FLASH_MISO_Port     GPIOB       /* MISO GPIO 端口。 */
#define FLASH_MOSI_Pin      GPIO_PIN_5  /* 软件 SPI 主出从入信号。 */
#define FLASH_MOSI_Port     GPIOB       /* MOSI GPIO 端口。 */

#define CMD_WRITE_ENABLE    0x06U /* 写使能命令。 */
#define CMD_READ_SR1        0x05U /* 读取状态寄存器 1。 */
#define CMD_READ_DATA       0x03U /* 标准低速读数据命令。 */
#define CMD_PAGE_PROGRAM    0x02U /* 页编程命令，最多连续写 256 字节。 */
#define CMD_SECTOR_ERASE    0x20U /* 4KB 扇区擦除命令。 */
#define CMD_MANUF_DEVICE_ID 0x90U /* 读取制造商/设备 ID 命令。 */

#define GAME_RECORD_MAGIC   0x54524753UL /* 最近游戏记录有效性标记。 */

static uint16_t flash_id; /* 初始化时读取到的 FLASH ID，用于判断芯片是否在线。 */

typedef struct {
    uint32_t magic;                    /* 固定标记，避免误把空 FLASH 当作记录。 */
    spi_flash_game_record_t record;    /* 真正保存的游戏快照。 */
    uint16_t checksum;                 /* 对 magic 和 record 计算的 16 位校验。 */
} flash_record_blob_t;

/* 使用软件 SPI，避免与 I2S/SPI2 音频播放通路互相占用。 */
static void flash_delay(void)
{
    __NOP();
    __NOP();
    __NOP();
}

/* 控制片选信号；high=1 释放 FLASH，high=0 选中 FLASH。 */
static void cs(uint8_t high)
{
    HAL_GPIO_WritePin(FLASH_CS_Port, FLASH_CS_Pin, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* 软件 SPI 发送 1 字节并同时采样 1 字节，模式接近 CPOL=0/CPHA=0。 */
static uint8_t spi_xfer(uint8_t out)
{
    uint8_t in = 0;

    for (uint8_t i = 0; i < 8; i++) {
        HAL_GPIO_WritePin(FLASH_SCK_Port, FLASH_SCK_Pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(FLASH_MOSI_Port, FLASH_MOSI_Pin,
                          (out & 0x80U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        out <<= 1;
        flash_delay();
        HAL_GPIO_WritePin(FLASH_SCK_Port, FLASH_SCK_Pin, GPIO_PIN_SET);
        in <<= 1;
        if (HAL_GPIO_ReadPin(FLASH_MISO_Port, FLASH_MISO_Pin) == GPIO_PIN_SET) {
            in |= 1U;
        }
        flash_delay();
    }

    HAL_GPIO_WritePin(FLASH_SCK_Port, FLASH_SCK_Pin, GPIO_PIN_RESET);
    return in;
}

/* 按 FLASH 命令格式发送 24 位地址，高字节在前。 */
static void send_addr(uint32_t addr)
{
    spi_xfer((uint8_t)(addr >> 16));
    spi_xfer((uint8_t)(addr >> 8));
    spi_xfer((uint8_t)addr);
}

/* 读取状态寄存器 1，用于判断写入/擦除是否仍在忙。 */
static uint8_t read_sr1(void)
{
    uint8_t sr;

    cs(0);
    spi_xfer(CMD_READ_SR1);
    sr = spi_xfer(0xFF);
    cs(1);
    return sr;
}

/* 等待 FLASH 内部写入或擦除完成。 */
static void wait_busy(void)
{
    /* NOR FLASH 的写入/擦除命令内部自定时，这里轮询 WIP 位等待完成。 */
    while ((read_sr1() & 0x01U) != 0U) {
    }
}

/* 写入/擦除前必须先发写使能命令，否则 FLASH 会忽略修改操作。 */
static void write_enable(void)
{
    cs(0);
    spi_xfer(CMD_WRITE_ENABLE);
    cs(1);
}

/* 简单 16 位滚动异或校验，用于发现记录损坏或未初始化。 */
static uint16_t checksum16(const uint8_t *buf, uint16_t len)
{
    uint16_t sum = 0x5A5AU;

    for (uint16_t i = 0; i < len; i++) {
        sum = (uint16_t)((sum << 3) | (sum >> 13));
        sum ^= buf[i];
    }

    return sum;
}

/* 初始化软件 SPI GPIO，并读取芯片 ID 缓存到 flash_id。 */
void SpiFlash_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitStruct.Pin = FLASH_CS_Pin | FLASH_SCK_Pin | FLASH_MOSI_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = FLASH_MISO_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    cs(1);
    HAL_GPIO_WritePin(FLASH_SCK_Port, FLASH_SCK_Pin, GPIO_PIN_RESET);
    flash_id = SpiFlash_ReadID();
}

/* 通过 0x90 命令读取制造商和设备 ID。 */
uint16_t SpiFlash_ReadID(void)
{
    uint16_t id;

    cs(0);
    spi_xfer(CMD_MANUF_DEVICE_ID);
    spi_xfer(0);
    spi_xfer(0);
    spi_xfer(0);
    id = (uint16_t)spi_xfer(0xFF) << 8;
    id |= spi_xfer(0xFF);
    cs(1);
    return id;
}

/* 判断初始化时读取到的 ID 是否不是 0x0000/0xFFFF。 */
uint8_t SpiFlash_IsReady(void)
{
    return flash_id != 0x0000U && flash_id != 0xFFFFU;
}

/* 从任意地址顺序读取 len 字节，调用者需保证 buf 有足够空间。 */
void SpiFlash_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    cs(0);
    spi_xfer(CMD_READ_DATA);
    send_addr(addr);
    while (len-- != 0U) {
        *buf++ = spi_xfer(0xFF);
    }
    cs(1);
}

/* 擦除 addr 所在扇区；调用前不要求地址按扇区对齐。 */
void SpiFlash_EraseSector(uint32_t addr)
{
    write_enable();
    cs(0);
    spi_xfer(CMD_SECTOR_ERASE);
    send_addr(addr);
    cs(1);
    wait_busy();
}

/* 写入任意长度数据，内部按 256 字节页边界拆分。 */
void SpiFlash_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    /* Page Program 不能跨越 256 字节页边界，因此按页拆分写入。 */
    while (len != 0U) {
        uint32_t page_space = 256U - (addr & 0xFFU);
        uint32_t chunk = (len < page_space) ? len : page_space;

        write_enable();
        cs(0);
        spi_xfer(CMD_PAGE_PROGRAM);
        send_addr(addr);
        for (uint32_t i = 0; i < chunk; i++) {
            spi_xfer(buf[i]);
        }
        cs(1);
        wait_busy();

        addr += chunk;
        buf += chunk;
        len -= chunk;
    }
}

/* 保存最近游戏快照到固定用户区域。 */
uint8_t SpiFlash_SaveGameRecord(const spi_flash_game_record_t *record)
{
    flash_record_blob_t blob = {0};

    if (!SpiFlash_IsReady() || record == 0) {
        return 0;
    }

    blob.magic = GAME_RECORD_MAGIC;
    blob.record = *record;
    blob.checksum = checksum16((const uint8_t *)&blob, offsetof(flash_record_blob_t, checksum));

    /* 保存的数据块位于一个扇区内，写入前先擦除以恢复为全 1。 */
    SpiFlash_EraseSector(SPI_FLASH_USER_ADDR);
    SpiFlash_Write(SPI_FLASH_USER_ADDR, (const uint8_t *)&blob, sizeof(blob));
    return 1;
}

/* 从固定用户区域读取最近游戏快照，并验证 magic/checksum。 */
uint8_t SpiFlash_LoadGameRecord(spi_flash_game_record_t *record)
{
    flash_record_blob_t blob;
    uint16_t crc;

    if (!SpiFlash_IsReady() || record == 0) {
        return 0;
    }

    SpiFlash_Read(SPI_FLASH_USER_ADDR, (uint8_t *)&blob, sizeof(blob));
    crc = checksum16((const uint8_t *)&blob, offsetof(flash_record_blob_t, checksum));
    if (blob.magic != GAME_RECORD_MAGIC || blob.checksum != crc) {
        return 0;
    }

    *record = blob.record;
    return 1;
}
