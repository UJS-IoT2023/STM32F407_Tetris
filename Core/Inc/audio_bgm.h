#ifndef __AUDIO_BGM_H
#define __AUDIO_BGM_H

#include "main.h"

/* BGM 播放模块：负责 Korobeiniki PCM 数据播放、ES8388 初始化、
 * I2S DMA 循环输出，以及菜单中的音量控制。
 */
void Audio_BGM_Init(void);
void Audio_BGM_Start(void);
void Audio_BGM_Stop(void);
void Audio_BGM_Task(void);
void Audio_BGM_DMA_IRQHandler(void);
void Audio_BGM_SetVolume(uint8_t volume);
uint8_t Audio_BGM_GetVolume(void);

#endif /* __AUDIO_BGM_H */
