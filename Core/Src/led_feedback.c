/**
 * @file led_feedback.c
 * @brief LED0/LED1 游戏状态反馈实现。
 *
 * 该模块把菜单、游戏中、消行和 Game Over 映射为不同 LED 闪烁模式。
 * 所有闪烁都基于 HAL_GetTick()，不会使用阻塞延时。
 */
#include "led_feedback.h"

#define LED_ON      GPIO_PIN_RESET /* 正点原子板载 LED 通常为低电平点亮。 */
#define LED_OFF     GPIO_PIN_SET   /* 高电平关闭 LED。 */

static led_feedback_state_t led_state = LED_FEEDBACK_MENU; /* 当前 LED 反馈状态。 */
static uint32_t led_last_tick;       /* 上一次 LED 翻转或状态刷新的 tick。 */
static uint32_t line_flash_until;    /* 消行动效结束时间，0 表示当前无消行闪烁。 */
static uint8_t blink_phase;          /* 菜单慢闪相位，0/1 交替控制 LED1。 */

/* 设置 LED0 输出电平，隐藏低电平点亮的硬件细节。 */
static void led0_write(GPIO_PinState state)
{
    HAL_GPIO_WritePin(LED_0_GPIO_Port, LED_0_Pin, state);
}

/* 设置 LED1 输出电平，隐藏低电平点亮的硬件细节。 */
static void led1_write(GPIO_PinState state)
{
    HAL_GPIO_WritePin(LED_1_GPIO_Port, LED_1_Pin, state);
}

/* 初始化 LED 输出，并进入菜单等待状态。 */
void Led_Feedback_Init(void)
{
    led0_write(LED_OFF);
    led1_write(LED_OFF);
    Led_Feedback_SetState(LED_FEEDBACK_MENU);
}

/* 切换 LED 状态；切换时重置计时，避免沿用上一个状态的闪烁节奏。 */
void Led_Feedback_SetState(led_feedback_state_t state)
{
    led_state = state;
    led_last_tick = HAL_GetTick();
    blink_phase = 0;

    if (state == LED_FEEDBACK_LINE_CLEAR) {
        /* 消行动画是短暂叠加效果，结束后自动回到 PLAYING 状态。 */
        line_flash_until = led_last_tick + 350U;
        return;
    }

    line_flash_until = 0;

    if (state == LED_FEEDBACK_MENU) {
        led0_write(LED_OFF);
        led1_write(LED_ON);
    } else if (state == LED_FEEDBACK_PLAYING) {
        led0_write(LED_OFF);
        led1_write(LED_ON);
    } else {
        led0_write(LED_ON);
        led1_write(LED_OFF);
    }
}

/* 周期刷新 LED 闪烁模式；主循环持续调用即可。 */
void Led_Feedback_Task(void)
{
    uint32_t now = HAL_GetTick();

    /* 非阻塞 LED 状态机，不影响 tetris_loop 的正常刷新。 */
    if (line_flash_until != 0U) {
        if (now >= line_flash_until) {
            line_flash_until = 0;
            Led_Feedback_SetState(LED_FEEDBACK_PLAYING);
        } else if (now - led_last_tick >= 60U) {
            led_last_tick = now;
            HAL_GPIO_TogglePin(LED_0_GPIO_Port, LED_0_Pin);
            HAL_GPIO_TogglePin(LED_1_GPIO_Port, LED_1_Pin);
        }
        return;
    }

    if (led_state == LED_FEEDBACK_PLAYING) {
        if (now - led_last_tick >= 500U) {
            led_last_tick = now;
            HAL_GPIO_TogglePin(LED_1_GPIO_Port, LED_1_Pin);
            led0_write(LED_OFF);
        }
    } else if (led_state == LED_FEEDBACK_MENU) {
        if (now - led_last_tick >= 900U) {
            led_last_tick = now;
            blink_phase ^= 1U;
            led1_write(blink_phase ? LED_ON : LED_OFF);
            led0_write(LED_OFF);
        }
    } else if (led_state == LED_FEEDBACK_GAME_OVER) {
        if (now - led_last_tick >= 180U) {
            led_last_tick = now;
            HAL_GPIO_TogglePin(LED_0_GPIO_Port, LED_0_Pin);
            led1_write(LED_OFF);
        }
    }
}
