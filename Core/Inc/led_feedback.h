#ifndef __LED_FEEDBACK_H
#define __LED_FEEDBACK_H

#include "main.h"

/* LED0/LED1 用不同闪烁模式反馈当前游戏状态。 */
typedef enum {
    LED_FEEDBACK_MENU = 0,
    LED_FEEDBACK_PLAYING,
    LED_FEEDBACK_LINE_CLEAR,
    LED_FEEDBACK_GAME_OVER,
} led_feedback_state_t;

void Led_Feedback_Init(void);
void Led_Feedback_SetState(led_feedback_state_t state);
void Led_Feedback_Task(void);

#endif /* __LED_FEEDBACK_H */
