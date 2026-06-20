#include "spi_flash.h"

#include <stddef.h>

#define FLASH_CS_Pin        GPIO_PIN_14
#define FLASH_CS_Port       GPIOB
#define FLASH_SCK_Pin       GPIO_PIN_3
#define FLASH_SCK_Port      GPIOB
#define FLASH_MISO_Pin      GPIO_PIN_4
#define FLASH_MISO_Port     GPIOB
#define FLASH_MOSI_Pin      GPIO_PIN_5
#define FLASH_MOSI_Port     GPIOB

#define CMD_WRITE_ENABLE    0x06U
#define CMD_READ_SR1        0x05U
#define CMD_READ_DATA       0x03U
#define CMD_PAGE_PROGRAM    0x02U
#define CMD_SECTOR_ERASE    0x20U
#define CMD_MANUF_DEVICE_ID 0x90U

#define GAME_RECORD_MAGIC   0x54524753UL

static uint16_t flash_id;

typedef struct {
    uint32_t magic;
    spi_flash_game_record_t record;
    uint16_t checksum;
} flash_record_blob_t;

/* 使用软件 SPI，避免与 I2S/SPI2 音频播放通路互相占用。 */
static void flash_delay(void)
{
    __NOP();
    __NOP();
    __NOP();
}

static void cs(uint8_t high)
{
    HAL_GPIO_WritePin(FLASH_CS_Port, FLASH_CS_Pin, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

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

static void send_addr(uint32_t addr)
{
    spi_xfer((uint8_t)(addr >> 16));
    spi_xfer((uint8_t)(addr >> 8));
    spi_xfer((uint8_t)addr);
}

static uint8_t read_sr1(void)
{
    uint8_t sr;

    cs(0);
    spi_xfer(CMD_READ_SR1);
    sr = spi_xfer(0xFF);
    cs(1);
    return sr;
}

static void wait_busy(void)
{
    /* NOR FLASH 的写入/擦除命令内部自定时，这里轮询 WIP 位等待完成。 */
    while ((read_sr1() & 0x01U) != 0U) {
    }
}

static void write_enable(void)
{
    cs(0);
    spi_xfer(CMD_WRITE_ENABLE);
    cs(1);
}

static uint16_t checksum16(const uint8_t *buf, uint16_t len)
{
    uint16_t sum = 0x5A5AU;

    for (uint16_t i = 0; i < len; i++) {
        sum = (uint16_t)((sum << 3) | (sum >> 13));
        sum ^= buf[i];
    }

    return sum;
}

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

uint8_t SpiFlash_IsReady(void)
{
    return flash_id != 0x0000U && flash_id != 0xFFFFU;
}

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

void SpiFlash_EraseSector(uint32_t addr)
{
    write_enable();
    cs(0);
    spi_xfer(CMD_SECTOR_ERASE);
    send_addr(addr);
    cs(1);
    wait_busy();
}

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
