#include "led_feedback.h"

#define LED_ON      GPIO_PIN_RESET
#define LED_OFF     GPIO_PIN_SET

static led_feedback_state_t led_state = LED_FEEDBACK_MENU;
static uint32_t led_last_tick;
static uint32_t line_flash_until;
static uint8_t blink_phase;

static void led0_write(GPIO_PinState state)
{
    HAL_GPIO_WritePin(LED_0_GPIO_Port, LED_0_Pin, state);
}

static void led1_write(GPIO_PinState state)
{
    HAL_GPIO_WritePin(LED_1_GPIO_Port, LED_1_Pin, state);
}

void Led_Feedback_Init(void)
{
    led0_write(LED_OFF);
    led1_write(LED_OFF);
    Led_Feedback_SetState(LED_FEEDBACK_MENU);
}

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
