/**
 * @file audio_bgm.h
 * @brief BGM 播放接口，封装 ES8388 初始化、I2S DMA 输出和音量控制。
 *
 * 游戏模块通过 Start/Stop 控制音乐播放，通过 SetVolume/GetVolume
 * 和 EEPROM 设置模块联动，实现复位后保持用户音量设置。
 */
#ifndef __AUDIO_BGM_H
#define __AUDIO_BGM_H

#include "main.h"

/* BGM 播放模块：负责 Korobeiniki PCM 数据播放、ES8388 初始化、
 * I2S DMA 循环输出，以及菜单中的音量控制。
 */
void Audio_BGM_Init(void);
/* 从头开始播放 BGM；实际音频数据在 DMA 回调中持续填充。 */
void Audio_BGM_Start(void);
/* 停止 BGM 输出，DMA 仍运行但缓冲区填充为静音。 */
void Audio_BGM_Stop(void);
/* 预留的周期任务接口，当前 DMA 播放不需要主循环轮询处理。 */
void Audio_BGM_Task(void);
/* DMA1 Stream4 中断入口的应用层封装，由中断文件调用。 */
void Audio_BGM_DMA_IRQHandler(void);
/* 设置用户音量档位，范围 0..10，实际输出会继续按安全比例限幅。 */
void Audio_BGM_SetVolume(uint8_t volume);
/* 获取当前用户音量档位，用于菜单显示和 EEPROM 保存。 */
uint8_t Audio_BGM_GetVolume(void);

#endif /* __AUDIO_BGM_H */
