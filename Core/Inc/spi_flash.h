#ifndef __SPI_FLASH_H
#define __SPI_FLASH_H

#include "main.h"

/* 外部 SPI FLASH 的预留区域。当前 BGM 仍以 C 数组放在片内 Flash，
 * 用户区域用于保存一份最近游戏快照。
 */
#define SPI_FLASH_BGM_ADDR      0x000000UL
#define SPI_FLASH_USER_ADDR     0x100000UL

/* 写入 SPI FLASH 的轻量级游戏结果记录。 */
typedef struct {
    uint32_t score;
    uint32_t lines;
    uint32_t level;
    uint8_t volume;
} spi_flash_game_record_t;

void SpiFlash_Init(void);
uint16_t SpiFlash_ReadID(void);
uint8_t SpiFlash_IsReady(void);
void SpiFlash_Read(uint32_t addr, uint8_t *buf, uint32_t len);
void SpiFlash_EraseSector(uint32_t addr);
void SpiFlash_Write(uint32_t addr, const uint8_t *buf, uint32_t len);
uint8_t SpiFlash_SaveGameRecord(const spi_flash_game_record_t *record);
uint8_t SpiFlash_LoadGameRecord(spi_flash_game_record_t *record);

#endif /* __SPI_FLASH_H */
