#ifndef __AUDIO_BGM_H
#define __AUDIO_BGM_H

#include "main.h"

void Audio_BGM_Init(void);
void Audio_BGM_Start(void);
void Audio_BGM_Stop(void);
void Audio_BGM_Task(void);
void Audio_BGM_DMA_IRQHandler(void);
void Audio_BGM_SetVolume(uint8_t volume);
uint8_t Audio_BGM_GetVolume(void);

#endif /* __AUDIO_BGM_H */
