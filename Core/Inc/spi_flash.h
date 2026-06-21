/**
 * @file spi_flash.h
 * @brief 外部 SPI FLASH 应用接口，当前用于保存最近一局游戏快照。
 *
 * 模块内部使用软件 SPI，避免占用音频使用的 I2S/SPI2 外设。
 */
#ifndef __SPI_FLASH_H
#define __SPI_FLASH_H

#include "main.h"

/* 外部 SPI FLASH 的预留区域。当前 BGM 仍以 C 数组放在片内 Flash，
 * 用户区域用于保存一份最近游戏快照。
 */
#define SPI_FLASH_BGM_ADDR      0x000000UL  /* 预留的 BGM 数据区域起始地址。 */
#define SPI_FLASH_USER_ADDR     0x100000UL  /* 用户数据区域，当前保存最近一局快照。 */

/* 写入 SPI FLASH 的轻量级游戏结果记录。 */
typedef struct {
    uint32_t score;  /* 最近一局游戏分数。 */
    uint32_t lines;  /* 最近一局累计消除行数。 */
    uint32_t level;  /* 最近一局结束时的等级。 */
    uint8_t volume;  /* 最近一局结束时的 BGM 音量档位。 */
} spi_flash_game_record_t;

/* 初始化软件 SPI 引脚，并读取 FLASH ID 判断芯片是否在线。 */
void SpiFlash_Init(void);
/* 读取制造商和设备 ID，高字节通常为制造商 ID。 */
uint16_t SpiFlash_ReadID(void);
/* 根据初始化阶段读取到的 ID 判断 FLASH 是否可用。 */
uint8_t SpiFlash_IsReady(void);
/* 从指定地址连续读取 len 字节数据。 */
void SpiFlash_Read(uint32_t addr, uint8_t *buf, uint32_t len);
/* 擦除指定地址所在的 4KB 扇区。 */
void SpiFlash_EraseSector(uint32_t addr);
/* 从指定地址写入 len 字节数据，内部会按 256 字节页自动拆分。 */
void SpiFlash_Write(uint32_t addr, const uint8_t *buf, uint32_t len);
/* 保存最近一局游戏快照，内部附加 magic 和 checksum。 */
uint8_t SpiFlash_SaveGameRecord(const spi_flash_game_record_t *record);
/* 读取最近一局游戏快照，并校验 magic/checksum 是否有效。 */
uint8_t SpiFlash_LoadGameRecord(spi_flash_game_record_t *record);

#endif /* __SPI_FLASH_H */
