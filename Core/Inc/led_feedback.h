/**
 * @file led_feedback.h
 * @brief LED0/LED1 游戏状态反馈接口。
 *
 * LED 模块以非阻塞状态机方式运行，主循环只需周期调用
 * Led_Feedback_Task()，不会打断游戏按键扫描和屏幕刷新。
 */
#ifndef __LED_FEEDBACK_H
#define __LED_FEEDBACK_H

#include "main.h"

/* LED0/LED1 用不同闪烁模式反馈当前游戏状态。 */
typedef enum {
    LED_FEEDBACK_MENU = 0,      /* 开始菜单状态：LED1 慢闪，提示等待开始。 */
    LED_FEEDBACK_PLAYING,       /* 游戏进行状态：LED1 周期闪烁，提示运行中。 */
    LED_FEEDBACK_LINE_CLEAR,    /* 消行提示状态：LED0/LED1 短暂快速闪烁。 */
    LED_FEEDBACK_GAME_OVER,     /* 游戏结束状态：LED0 快闪，提示本局结束。 */
} led_feedback_state_t;

/* 初始化 LED 输出状态，并切换到菜单反馈模式。 */
void Led_Feedback_Init(void);
/* 设置新的反馈状态，会重置闪烁计时和阶段。 */
void Led_Feedback_SetState(led_feedback_state_t state);
/* 周期性刷新 LED 状态机，建议在 main while(1) 中持续调用。 */
void Led_Feedback_Task(void);

#endif /* __LED_FEEDBACK_H */
