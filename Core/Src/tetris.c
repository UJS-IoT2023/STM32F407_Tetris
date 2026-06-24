/**
 * @file tetris.c
 * @brief 俄罗斯方块游戏主逻辑、GUI 绘制、菜单状态机和结算保存。
 *
 * 本文件是项目的核心应用层：负责方块移动、旋转、消行和计分，
 * 同时联动 BGM、LED、EEPROM、RTC 和 SPI FLASH 等硬件功能。
 */
#include "tetris.h"
#include "audio_bgm.h"
#include "app_rtc.h"
#include "app_settings.h"
#include "led_feedback.h"
#include "spi_flash.h"
#include <stdlib.h>
#include <string.h>
/* 红外解码结果由 stm32f4xx_it.c 的中断逻辑更新。 */
extern volatile uint8_t  ir_ready; /* 红外解码完成标志，1 表示 ir_code 中有新键值。 */
extern volatile uint32_t ir_code;  /* 最近一次红外 NEC 解码得到的键值。 */

/* ===================================================================
 *  俄罗斯方块形状数据，使用 4x4 位图描述。
 *  第 j 行、第 i 列对应 bit(j*4+i)，便于旋转形态查表。
 * =================================================================== */
const uint16_t shapes[7][4] = {
    /* I */ { 0x00F0, 0x2222, 0x00F0, 0x2222 },
    /* T */ { 0x0072, 0x0262, 0x0270, 0x0232 },
    /* S */ { 0x0063, 0x0264, 0x0063, 0x0264 },
    /* Z */ { 0x0462, 0x006C, 0x0462, 0x006C },
    /* J */ { 0x0446, 0x02E0, 0x0622, 0x0074 },
    /* L */ { 0x0226, 0x00E2, 0x0644, 0x0470 },
    /* O */ { 0x0660, 0x0660, 0x0660, 0x0660 },
};

/* 颜色索引：0 为空格，1..7 分别对应七种方块。 */
const uint16_t COLOR_MAP[8] = {
    WHITE,   /* 0 背景 */
    CYAN,    /* 1 I */
    MAGENTA, /* 2 T */
    GREEN,   /* 3 S */
    RED,     /* 4 Z */
    BLUE,    /* 5 J */
    BRRED,   /* 6 L */
    YELLOW,  /* 7 O */
};

/* ---- 游戏状态 ---------------------------------------------------- */
static uint8_t board[BOARD_ROWS][BOARD_COLS]; /* 棋盘内容，0 为空格，1..7 为颜色索引。 */
static int8_t piece_type;                      /* 当前活动方块类型，范围 0..6。 */
static int8_t next_type;                       /* 下一块方块类型，用于 NEXT 预览。 */
static int8_t piece_rot;                       /* 当前活动方块旋转形态，范围 0..3。 */
static int8_t piece_x;                         /* 当前活动方块 4x4 位图左上角 X 棋盘坐标。 */
static int8_t piece_y;                         /* 当前活动方块 4x4 位图左上角 Y 棋盘坐标。 */
static uint32_t score;                         /* 当前局得分。 */
static uint32_t lines_total;                   /* 当前局累计消除行数。 */
static uint32_t level;                         /* 当前等级，由消除行数推导。 */
static uint32_t fall_speed;                    /* 自动下落间隔，单位 ms。 */
static uint32_t last_fall_tick;                /* 上一次自动下落的 HAL tick。 */
static uint32_t last_key_tick;                 /* 上一次处理按键的 HAL tick，用于消抖。 */
static uint8_t game_state;                     /* 当前游戏状态：菜单、游戏中或结束。 */
static int8_t ghost_y;                         /* 影子方块最终落点的 Y 坐标。 */
static uint8_t menu_volume_drawn = 0xFF;       /* 上一次绘制的菜单音量，0xFF 强制首次刷新。 */
static uint8_t menu_history_visible;           /* 菜单中是否正在显示 EEPROM 历史页。 */
static uint8_t menu_history_page;              /* EEPROM 历史页当前页码，从 0 开始。 */
static uint32_t game_start_tick;               /* 本局开始时的 HAL tick，用于计算持续时间。 */
static app_datetime_t game_start_time;         /* 本局开始时的 RTC 时间，用于写入历史记录。 */
static const uint16_t *beep_pattern;           /* 当前蜂鸣器音效节奏表，奇偶段交替开/关。 */
static uint8_t beep_pattern_len;               /* 当前音效节奏表的段数。 */
static uint8_t beep_pattern_pos;               /* 当前播放到节奏表中的第几段。 */
static uint8_t beep_priority;                  /* 当前音效优先级，避免低优先级音效打断高优先级音效。 */
static uint8_t beep_output_on;                 /* 当前蜂鸣器输出状态，1 表示正在拉高发声。 */
static uint32_t beep_next_tick;                /* 当前蜂鸣器节奏段结束的 HAL tick。 */

#define STATE_MENU       0 /* 开始菜单状态。 */
#define STATE_PLAYING    1 /* 游戏进行中状态。 */
#define STATE_GAME_OVER  2 /* 游戏结束结算状态。 */

#define BEEP_EFFECT_MOVE       1U /* 方块移动音效，短促单击。 */
#define BEEP_EFFECT_ROTATE     2U /* 方块旋转音效，略长单击。 */
#define BEEP_EFFECT_DROP       3U /* 硬降音效，两段快速提示。 */
#define BEEP_EFFECT_LINE       4U /* 消行音效，三段上扬式节奏。 */
#define BEEP_EFFECT_GAME_OVER  5U /* 游戏结束音效，较长的下降式节奏。 */

/* ---- 通用辅助函数 ------------------------------------------------ */
/* 返回指定方块类型和旋转状态对应的 16 位 4x4 位图。 */
static inline uint16_t shape_word(int type, int rot)
{
    return shapes[type][rot];
}

/* 判断方块 4x4 位图中第 i 列、第 j 行是否有格子。 */
static inline int cell_set(int type, int rot, int i, int j)
{
    return (shape_word(type, rot) >> (j * 4 + i)) & 1;
}

/* 将 RGB565 颜色按比例压暗，用于绘制方块阴影和边框。 */
static uint16_t dim_color(uint16_t color)
{
    uint16_t r = (uint16_t)((color >> 11) & 0x1FU);
    uint16_t g = (uint16_t)((color >> 5) & 0x3FU);
    uint16_t b = (uint16_t)(color & 0x1FU);

    r = (uint16_t)((r * 3U) / 5U);
    g = (uint16_t)((g * 3U) / 5U);
    b = (uint16_t)((b * 3U) / 5U);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* 绘制带浅色投影和黑色边框的小面板。 */
static void draw_panel_box(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t fill)
{
    LCD_Fill(x1 + 1U, y1 + 1U, x2 - 1U, y2 - 1U, fill);
    POINT_COLOR = LGRAY;
    LCD_DrawRectangle(x1 + 1U, y1 + 1U, x2 + 1U, y2 + 1U);
    POINT_COLOR = BLACK;
    LCD_DrawRectangle(x1, y1, x2, y2);
}

/* 绘制菜单/结算页面中的矩形按钮；inverted=1 使用黑底白字。 */
static void draw_button(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                        const char *text, uint8_t inverted)
{
    uint16_t fill = inverted ? BLACK : WHITE;
    uint16_t text_color = inverted ? WHITE : BLACK;

    LCD_Fill(x1, y1, x2, y2, fill);
    POINT_COLOR = inverted ? GRAY : BLACK;
    LCD_DrawRectangle(x1, y1, x2, y2);
    POINT_COLOR = text_color;
    BACK_COLOR = fill;
    LCD_ShowString(x1 + 7U, y1 + 6U, (uint16_t)(x2 - x1 - 10U), 12, 12, (uint8_t *)text);
    BACK_COLOR = WHITE;
}

/* ---- 底层绘制 ---------------------------------------------------- */

/* 在棋盘坐标 (gx, gy) 绘制一个小方块。 */
static void draw_block(uint8_t gx, uint8_t gy, uint8_t ci)
{
    uint16_t px1 = BOARD_X_OFF + gx * BLOCK_SIZE;
    uint16_t py1 = BOARD_Y_OFF + gy * BLOCK_SIZE;
    uint16_t px2 = px1 + BLOCK_SIZE - 1;
    uint16_t py2 = py1 + BLOCK_SIZE - 1;

    if (ci == 0) {
        LCD_Fill(px1, py1, px2, py2, WHITE);
        POINT_COLOR = 0xEF7D;
        LCD_DrawRectangle(px1, py1, px2, py2);
    } else {
        uint16_t base = COLOR_MAP[ci];
        uint16_t dark = dim_color(base);

        LCD_Fill(px1 + 1U, py1 + 1U, px2 - 1U, py2 - 1U, base);
        LCD_Fill(px1 + 3U, py1 + 3U, px2 - 4U, py1 + 4U, WHITE);
        /* 左上高光，增强小方块立体感。 */
        POINT_COLOR = WHITE;
        LCD_DrawLine(px1, py1, px2, py1);
        LCD_DrawLine(px1, py1, px1, py2);
        /* 右下阴影。 */
        POINT_COLOR = dark;
        LCD_DrawLine(px1, py2, px2, py2);
        LCD_DrawLine(px2, py1, px2, py2);
        LCD_DrawRectangle(px1 + 1U, py1 + 1U, px2 - 1U, py2 - 1U);
    }
}

/* 绘制右侧 NEXT 预览框中的小方块。 */
static void draw_next_block(uint8_t gx, uint8_t gy, uint8_t ci)
{
    uint16_t px1 = PANEL_X + gx * NEXT_SIZE;
    uint16_t py1 = NEXT_Y  + gy * NEXT_SIZE;
    uint16_t px2 = px1 + NEXT_SIZE - 1;
    uint16_t py2 = py1 + NEXT_SIZE - 1;
    if (ci == 0U) {
        LCD_Fill(px1, py1, px2, py2, WHITE);
    } else {
        LCD_Fill(px1 + 1U, py1 + 1U, px2 - 1U, py2 - 1U, COLOR_MAP[ci]);
        POINT_COLOR = dim_color(COLOR_MAP[ci]);
        LCD_DrawRectangle(px1, py1, px2, py2);
    }
}

/* ---- 棋盘和方块绘制 ---------------------------------------------- */

/* 按 board 数组完整重绘棋盘，常用于开局、硬降和消行后刷新。 */
static void draw_board_full(void)
{
    for (int r = 0; r < BOARD_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++)
            draw_block(c, r, board[r][c]);
}

/* 擦除当前活动方块，恢复其下方已有棋盘格。 */
static void erase_piece(void)
{
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            if (cell_set(piece_type, piece_rot, i, j)) {
                int bx = piece_x + i;
                int by = piece_y + j;
                if (bx >= 0 && bx < BOARD_COLS && by >= 0 && by < BOARD_ROWS)
                    draw_block(bx, by, board[by][bx]);
            }
}

/* 绘制当前活动方块。 */
static void draw_piece(void)
{
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            if (cell_set(piece_type, piece_rot, i, j)) {
                int bx = piece_x + i;
                int by = piece_y + j;
                if (bx >= 0 && bx < BOARD_COLS && by >= 0 && by < BOARD_ROWS)
                    draw_block(bx, by, piece_type + 1);
            }
}

/* 绘制下一个方块预览框。 */
static void draw_next_piece(void)
{
    /* 先清空预览区域，避免上一块残影。 */
    LCD_Fill(PANEL_X, NEXT_Y,
             PANEL_X + 4 * NEXT_SIZE - 1,
             NEXT_Y  + 4 * NEXT_SIZE - 1, WHITE);
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            if (cell_set(next_type, 0, i, j))
                draw_next_block(i, j, next_type + 1);
}

/* ---- 分数和等级显示 ---------------------------------------------- */

/* 只刷新右侧 SCORE/LINES/LEVEL 数值区域，减少整屏重绘闪烁。 */
static void draw_score_panel(void)
{
    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
    LCD_Fill(PANEL_X, SCORE_Y, PANEL_X + 70, SCORE_Y + 12, WHITE);
    LCD_Fill(PANEL_X, LINES_Y, PANEL_X + 70, LINES_Y + 12, WHITE);
    LCD_Fill(PANEL_X, LEVEL_Y, PANEL_X + 32, LEVEL_Y + 12, WHITE);
    LCD_ShowxNum(PANEL_X, SCORE_Y, score,       5, 12, 0x80);
    LCD_ShowxNum(PANEL_X, LINES_Y, lines_total, 4, 12, 0x80);
    LCD_ShowxNum(PANEL_X, LEVEL_Y, level,       2, 12, 0x80);
}

/* ---- 碰撞检测 ---------------------------------------------------- */

/* 判断活动方块放在指定坐标和旋转状态时是否越界或撞到已有方块。 */
static int check_hit(int x, int y, int rot)
{
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            if (cell_set(piece_type, rot, i, j)) {
                int bx = x + i;
                int by = y + j;
                if (bx < 0 || bx >= BOARD_COLS || by >= BOARD_ROWS)
                    return 1;
                if (by >= 0 && board[by][bx])
                    return 1;
            }
    return 0;
}

/* ---- 固定当前方块到棋盘 ------------------------------------------ */

/* 将当前活动方块写入 board，表示方块已经落定。 */
static void merge_piece(void)
{
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            if (cell_set(piece_type, piece_rot, i, j)) {
                int bx = piece_x + i;
                int by = piece_y + j;
                if (by >= 0 && by < BOARD_ROWS && bx >= 0 && bx < BOARD_COLS)
                    board[by][bx] = piece_type + 1;
            }
}

/* ---- 消行逻辑 ---------------------------------------------------- */

/* 蜂鸣器音效函数前向声明，消行逻辑定义在音效实现之前。 */
static void beep_play(uint8_t effect);

/* 找到指定方块旋转状态最右侧占用列，用于靠墙旋转修正。 */
static int rightmost_col(int type, int rot)
{
    for (int c = 3; c >= 0; c--)
        for (int r = 0; r < 4; r++)
            if (cell_set(type, rot, c, r))
                return c;
    return 0;
}

/* 消行前的短暂闪烁动效，用于提示玩家该行被清除。 */
static void flash_line(uint8_t row)
{
    uint16_t x1 = BOARD_X_OFF;
    uint16_t y1 = (uint16_t)(BOARD_Y_OFF + row * BLOCK_SIZE);
    uint16_t x2 = (uint16_t)(BOARD_X_OFF + BOARD_COLS * BLOCK_SIZE - 1U);
    uint16_t y2 = (uint16_t)(y1 + BLOCK_SIZE - 1U);

    LCD_Fill(x1, y1, x2, y2, WHITE);
    HAL_Delay(28);
    for (uint8_t c = 0; c < BOARD_COLS; c++) {
        draw_block(c, row, board[row][c]);
    }
    HAL_Delay(28);
    LCD_Fill(x1, y1, x2, y2, YELLOW);
    HAL_Delay(28);
}

/* 检查满行、执行消除、更新分数等级，并触发 LED 消行反馈。 */
static void remove_lines(void)
{
    int cleared = 0;
    for (int r = BOARD_ROWS - 1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < BOARD_COLS; c++)
            if (!board[r][c]) { full = 0; break; }
        if (full) {
            cleared++;
            flash_line((uint8_t)r);
            /* 将上方行整体下移。 */
            for (int rr = r; rr > 0; rr--)
                memcpy(board[rr], board[rr - 1], BOARD_COLS);
            memset(board[0], 0, BOARD_COLS);
            r++; /* 下移后需要重新检查当前行。 */
        }
    }
    if (cleared) {
        static const int pts[] = { 0, 100, 300, 500, 800 };
        score       += pts[cleared];
        lines_total += cleared;
        level        = lines_total / 10;
        fall_speed   = FALL_SPEED_INIT - level * 30;
        if (fall_speed < FALL_SPEED_MIN) fall_speed = FALL_SPEED_MIN;
        Led_Feedback_SetState(LED_FEEDBACK_LINE_CLEAR);
        beep_play(BEEP_EFFECT_LINE);
        draw_score_panel();
    }
}

/* ---- 生成新方块 -------------------------------------------------- */

/* 从 next_type 生成新的活动方块，并随机准备下一块。 */
static void new_piece(void)
{
    piece_type = next_type;
    next_type  = rand() % 7;
    piece_rot  = 0;
    piece_x    = 3;
    piece_y    = 0;

    if (check_hit(piece_x, piece_y, piece_rot)) {
        game_state = STATE_GAME_OVER;
        return;
    }

    draw_next_piece();
}

/* ---- 蜂鸣器音效反馈 ---------------------------------------------- */

/* 关闭蜂鸣器输出，并清空当前音效状态。 */
static void beep_stop(void)
{
    HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_RESET);
    beep_output_on = 0;
    beep_pattern = 0;
    beep_pattern_len = 0;
    beep_pattern_pos = 0;
    beep_priority = 0;
}

/* 根据游戏动作启动不同的蜂鸣器节奏；优先级较低的音效不会打断高优先级音效。 */
static void beep_play(uint8_t effect)
{
    static const uint16_t move_pattern[] = { 12U };
    static const uint16_t rotate_pattern[] = { 24U };
    static const uint16_t drop_pattern[] = { 18U, 18U, 28U };
    static const uint16_t line_pattern[] = { 22U, 18U, 22U, 18U, 42U };
    static const uint16_t game_over_pattern[] = { 80U, 45U, 60U, 45U, 120U };
    const uint16_t *pattern = move_pattern;
    uint8_t len = sizeof(move_pattern) / sizeof(move_pattern[0]);
    uint8_t priority = effect;

    if (beep_pattern != 0 && beep_priority > priority) {
        return;
    }

    if (effect == BEEP_EFFECT_ROTATE) {
        pattern = rotate_pattern;
        len = sizeof(rotate_pattern) / sizeof(rotate_pattern[0]);
    } else if (effect == BEEP_EFFECT_DROP) {
        pattern = drop_pattern;
        len = sizeof(drop_pattern) / sizeof(drop_pattern[0]);
    } else if (effect == BEEP_EFFECT_LINE) {
        pattern = line_pattern;
        len = sizeof(line_pattern) / sizeof(line_pattern[0]);
    } else if (effect == BEEP_EFFECT_GAME_OVER) {
        pattern = game_over_pattern;
        len = sizeof(game_over_pattern) / sizeof(game_over_pattern[0]);
    }

    beep_pattern = pattern;
    beep_pattern_len = len;
    beep_pattern_pos = 0;
    beep_priority = priority;
    beep_output_on = 1;
    HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_SET);
    beep_next_tick = HAL_GetTick() + beep_pattern[0];
}

/* 非阻塞刷新蜂鸣器节奏，奇数段静音、偶数段发声。 */
static void beep_task(void)
{
    uint32_t now = HAL_GetTick();

    if (beep_pattern == 0 || now < beep_next_tick) {
        return;
    }

    beep_pattern_pos++;
    if (beep_pattern_pos >= beep_pattern_len) {
        beep_stop();
        return;
    }

    beep_output_on = (uint8_t)((beep_pattern_pos & 1U) == 0U);
    HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, beep_output_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    beep_next_tick = now + beep_pattern[beep_pattern_pos];
}

/* 影子方块擦除函数前向声明，按键处理中会在定义前调用。 */
static void erase_ghost(void);
/* 影子方块绘制函数前向声明，按键处理中会在定义前调用。 */
static void draw_ghost(void);

/* ---- 按键处理，非阻塞，20 ms 消抖 ------------------------------- */

/* 处理游戏进行中的板载按键，包含消抖、移动、旋转和硬降。 */
static void game_key_handler(void)
{
    uint32_t now = HAL_GetTick();
    if (now - last_key_tick < KEY_DEBOUNCE) return;
    last_key_tick = now;

    static uint8_t k0_lock, k1_lock, k2_lock, kup_lock;

    /* KEY0：左移。 */
    if (HAL_GPIO_ReadPin(KEY_2_GPIO_Port, KEY_2_Pin) == GPIO_PIN_RESET) {
        if (!k0_lock) {
            k0_lock = 1;
            if (!check_hit(piece_x - 1, piece_y, piece_rot)) {
                erase_ghost();
                erase_piece();
                piece_x--;
                draw_piece();
                draw_ghost();
                beep_play(BEEP_EFFECT_MOVE);
            }
        }
    } else k0_lock = 0;

    /* KEY2：右移。 */
    if (HAL_GPIO_ReadPin(KEY_0_GPIO_Port, KEY_0_Pin) == GPIO_PIN_RESET) {
        if (!k2_lock) {
            k2_lock = 1;
            if (!check_hit(piece_x + 1, piece_y, piece_rot)) {
                erase_ghost();
                erase_piece();
                piece_x++;
                draw_piece();
                draw_ghost();
                beep_play(BEEP_EFFECT_MOVE);
            }
        }
    } else k2_lock = 0;

    /* KEY1：旋转。 */
    if (HAL_GPIO_ReadPin(KEY_UP_GPIO_Port, KEY_UP_Pin) == GPIO_PIN_RESET) {
        if (!k1_lock) {
            k1_lock = 1;

            /* 先按旧位置擦除，再尝试应用旋转后的新位置。 */
            erase_ghost();
            erase_piece();

            int new_rot = (piece_rot + 1) & 3;
            int new_x   = piece_x;

            /* 旋转后如果右侧越界，先向左修正。 */
            while (new_x + rightmost_col(piece_type, new_rot) >= BOARD_COLS)
                new_x--;

            /* 如果仍然碰撞，再尝试向右微调 1 到 2 格。 */
            if (check_hit(new_x, piece_y, new_rot)) {
                int ok = 0;
                for (int kick = 1; kick <= 2; kick++) {
                    if (!check_hit(new_x + kick, piece_y, new_rot)) {
                        new_x += kick;
                        ok = 1;
                        break;
                    }
                }
                if (ok) {
                    piece_rot = new_rot;
                    piece_x   = new_x;
                }
                /* 仍不合法则回滚，保持原旋转和原位置。 */
            } else {
                piece_rot = new_rot;
                piece_x   = new_x;
            }

            draw_piece();
            draw_ghost();
            beep_play(BEEP_EFFECT_ROTATE);
        }
    } else k1_lock = 0;

   /* WK_UP：硬降到底。 */
    if (HAL_GPIO_ReadPin(KEY_1_GPIO_Port, KEY_1_Pin) == GPIO_PIN_RESET) {
        if (!kup_lock) {
            kup_lock = 1;
            erase_ghost();
            erase_piece();
            while (!check_hit(piece_x, piece_y + 1, piece_rot))
                piece_y++;
            merge_piece();
            draw_board_full();
            remove_lines();
            draw_board_full();     /* 消行搬移后重绘棋盘。 */
            beep_play(BEEP_EFFECT_DROP);
            new_piece();
            if (game_state == STATE_PLAYING) { draw_piece(); draw_ghost(); }
        }
    } else kup_lock = 0;
}

/* ---- 游戏结束页面 ------------------------------------------------ */

/* 开始菜单绘制函数前向声明，Game Over 返回菜单时会提前调用。 */
static void draw_start_screen(void);
/* 菜单音量条绘制函数前向声明，Game Over 返回菜单时会提前调用。 */
static void draw_menu_volume(void);

/* 绘制 Game Over 结算页，并保存 EEPROM 历史和 SPI FLASH 快照。 */
static void show_game_over(void)
{
    spi_flash_game_record_t last_record;
    uint32_t duration_seconds = (HAL_GetTick() - game_start_tick) / 1000U;

    Audio_BGM_Stop();
    Led_Feedback_SetState(LED_FEEDBACK_GAME_OVER);
    beep_play(BEEP_EFFECT_GAME_OVER);
    /* 游戏结束时同时写 EEPROM 历史记录和 SPI FLASH 最近一局快照。 */
    App_Settings_AddGameResult(score, lines_total, duration_seconds, &game_start_time);
    last_record.score = score;
    last_record.lines = lines_total;
    last_record.level = level;
    last_record.volume = Audio_BGM_GetVolume();
    (void)SpiFlash_SaveGameRecord(&last_record);
    draw_panel_box(BOARD_X_OFF + 4, BOARD_Y_OFF + 48,
                   BOARD_X_OFF + BOARD_COLS * BLOCK_SIZE - 5,
                   BOARD_Y_OFF + 202, WHITE);
    LCD_Fill(BOARD_X_OFF + 4, BOARD_Y_OFF + 48,
             BOARD_X_OFF + BOARD_COLS * BLOCK_SIZE - 5,
             BOARD_Y_OFF + 50, RED);
    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
    LCD_ShowString(BOARD_X_OFF + 10, BOARD_Y_OFF + 62, 120, 24, 24,
                   (uint8_t *)"GAME");
    LCD_ShowString(BOARD_X_OFF + 10, BOARD_Y_OFF + 92, 120, 24, 24,
                   (uint8_t *)"OVER");
    LCD_Fill(BOARD_X_OFF + 12, BOARD_Y_OFF + 122,
             BOARD_X_OFF + BOARD_COLS * BLOCK_SIZE - 14,
             BOARD_Y_OFF + 123, LGRAY);
    LCD_ShowString(BOARD_X_OFF + 12, BOARD_Y_OFF + 132, 72, 12, 12, (uint8_t *)"SCORE");
    LCD_ShowxNum(BOARD_X_OFF + 58, BOARD_Y_OFF + 132, score, 5, 12, 0x80);
    LCD_ShowString(BOARD_X_OFF + 12, BOARD_Y_OFF + 148, 72, 12, 12, (uint8_t *)"LINES");
    LCD_ShowxNum(BOARD_X_OFF + 58, BOARD_Y_OFF + 148, lines_total, 4, 12, 0x80);
    LCD_ShowString(BOARD_X_OFF + 12, BOARD_Y_OFF + 164, 72, 12, 12, (uint8_t *)"BEST");
    LCD_ShowxNum(BOARD_X_OFF + 58, BOARD_Y_OFF + 164, App_Settings_Get()->high_score, 5, 12, 0x80);
    draw_button(BOARD_X_OFF + 12, BOARD_Y_OFF + 182,
                BOARD_X_OFF + 60, BOARD_Y_OFF + 202, "KEY0", 1);
    draw_button(BOARD_X_OFF + 68, BOARD_Y_OFF + 182,
                BOARD_X_OFF + 112, BOARD_Y_OFF + 202, "MENU", 0);
}

/* 从 Game Over 返回开始菜单，方便查看记录或调整音量。 */
static void return_to_start_menu(void)
{
    game_state = STATE_MENU;
    menu_history_visible = 0;
    menu_history_page = 0;
    menu_volume_drawn = 0xFF;
    draw_start_screen();
    draw_menu_volume();
    Audio_BGM_Start();
    Led_Feedback_SetState(LED_FEEDBACK_MENU);
}

/* ---- 游戏主界面静态布局 ------------------------------------------ */

/* 绘制游戏主界面的固定元素：标题栏、棋盘框、右侧 HUD 和按键说明。 */
static void draw_static_ui(void)
{
    LCD_Init();
    LCD_Display_Dir(0);   /* 竖屏 240 x 320。 */
    LCD_Clear(WHITE);
    BACK_COLOR  = WHITE;

    /* 顶部标题。 */
    LCD_Fill(0, 0, 239, 30, BLACK);
    POINT_COLOR = WHITE;
    BACK_COLOR = BLACK;
    LCD_ShowString(8, 8, 140, 16, 16, (uint8_t *)"TETRIS");
    POINT_COLOR = CYAN;
    LCD_Fill(174, 12, 186, 20, CYAN);
    LCD_Fill(188, 12, 200, 20, MAGENTA);
    LCD_Fill(202, 12, 214, 20, GREEN);
    LCD_Fill(216, 12, 228, 20, YELLOW);
    BACK_COLOR = WHITE;

    /* 顶部分隔线。 */
    LCD_Fill(0, 30, 239, 31, LGRAY);

    /* 棋盘边框。 */
    draw_panel_box(BOARD_X_OFF - 3, BOARD_Y_OFF - 3,
                   BOARD_X_OFF + BOARD_COLS * BLOCK_SIZE + 2,
                   BOARD_Y_OFF + BOARD_ROWS * BLOCK_SIZE + 2, WHITE);

    /* 右侧信息面板。 */
    draw_panel_box(PANEL_X - 6, 40, 232, 246, WHITE);
    POINT_COLOR = BLACK;
    LCD_ShowString(PANEL_X, NEXT_Y - 16, 56, 12, 12, (uint8_t *)"NEXT");
    draw_panel_box(PANEL_X - 2, NEXT_Y - 2,
                   PANEL_X + 4 * NEXT_SIZE + 1,
                   NEXT_Y  + 4 * NEXT_SIZE + 1, WHITE);

    POINT_COLOR = BLACK;
    LCD_ShowString(PANEL_X, SCORE_Y - 14, 56, 12, 12, (uint8_t *)"SCORE");
    LCD_ShowString(PANEL_X, LINES_Y - 14, 56, 12, 12, (uint8_t *)"LINES");
    LCD_ShowString(PANEL_X, LEVEL_Y - 14, 56, 12, 12, (uint8_t *)"LEVEL");

    draw_panel_box(8, CTRL_Y - 6, 232, 302, WHITE);
    POINT_COLOR = GRAY;
    LCD_ShowString(16, CTRL_Y,      64, 12, 12, (uint8_t *)"K0 LEFT");
    LCD_ShowString(16, CTRL_Y + 16, 72, 12, 12, (uint8_t *)"K2 RIGHT");
    LCD_ShowString(116, CTRL_Y,     64, 12, 12, (uint8_t *)"K1 ROT");
    LCD_ShowString(116, CTRL_Y + 16, 64, 12, 12, (uint8_t *)"UP DROP");

    /* 底部分隔线。 */
    LCD_Fill(8, 306, 232, 307, LGRAY);
    POINT_COLOR = LGRAY;
    LCD_ShowString(8, 309, 144, 12, 12, (uint8_t *)"STM32F407");
}
/* 红外遥控动作也需要擦除/绘制影子方块，因此这里保留前向声明。 */
static void erase_ghost(void);
static void draw_ghost(void);


/* ---- 红外遥控按键轮询 -------------------------------------------- */

/* 读取一次红外键值；成功读取后清除 ir_ready，避免重复触发。 */
static uint8_t ir_read_key(uint32_t *code)

{

    uint8_t ret = 0;

    __disable_irq();

    if (ir_ready) { ir_ready = 0; *code = ir_code; ret = 1; }

    __enable_irq();

    return ret;

}

/* 将红外遥控键值映射为游戏动作。 */
static void ir_do_action(uint32_t code)

{

    switch (code) {

    case 0x00FF22DD:

        if (!check_hit(piece_x - 1, piece_y, piece_rot))

            { erase_ghost(); erase_piece(); piece_x--; draw_piece(); draw_ghost(); beep_play(BEEP_EFFECT_MOVE); } break;

    case 0x00FFC23D:

        if (!check_hit(piece_x + 1, piece_y, piece_rot))

            { erase_ghost(); erase_piece(); piece_x++; draw_piece(); draw_ghost(); beep_play(BEEP_EFFECT_MOVE); } break;

    case 0x00FF629D:

        { erase_ghost(); erase_piece();

          int nr = (piece_rot + 1) & 3, nx = piece_x;

          while (nx + rightmost_col(piece_type, nr) >= BOARD_COLS) nx--;

          if (check_hit(nx, piece_y, nr)) {

              for (int k = 1; k <= 2; k++)

                  if (!check_hit(nx+k, piece_y, nr)) { nx += k; break; }

              if (check_hit(nx, piece_y, nr)) { nr = piece_rot; nx = piece_x; }

          }

          piece_rot = nr; piece_x = nx; draw_piece(); draw_ghost(); beep_play(BEEP_EFFECT_ROTATE);

        } break;

    case 0x00FF02FD:

        { erase_ghost(); erase_piece();

          while (!check_hit(piece_x, piece_y + 1, piece_rot)) piece_y++;

          merge_piece(); draw_board_full(); remove_lines(); draw_board_full(); beep_play(BEEP_EFFECT_DROP);

          new_piece();

          if (game_state == STATE_PLAYING) { draw_piece(); draw_ghost(); }

          else { show_game_over(); }

        } break;

    }

}


/* ---- 开始菜单 ---------------------------------------------------- */

/* 绘制开始菜单，包括标题、音量、最高分、按键说明和历史入口。 */
static void draw_start_screen(void)

{

    static const uint16_t bar_c[] = { CYAN, MAGENTA, GREEN, YELLOW, RED, BLUE };

    LCD_Fill(0, BOARD_Y_OFF, 239, 305, WHITE);
    LCD_Fill(0, 34, 239, 38, BLACK);

    for (int i = 0; i < 6; i++) {
        LCD_Fill(14 + i * 18, 52, 28 + i * 18, 66, bar_c[i]);
        POINT_COLOR = dim_color(bar_c[i]);
        LCD_DrawRectangle(14 + i * 18, 52, 28 + i * 18, 66);
    }

    POINT_COLOR = BLACK; BACK_COLOR = WHITE;

    LCD_ShowString(14, 84, 112, 24, 24, (uint8_t *)"TETRIS");

    LCD_Fill(14, 114, 122, 116, BLACK);

    draw_panel_box(14, 130, 124, 190, WHITE);
    POINT_COLOR = GRAY;

    LCD_ShowString(24, 142, 90, 12, 12, (uint8_t *)"KEY0 START");

    LCD_ShowString(24, 158, 90, 12, 12, (uint8_t *)"KEY1 VOL-");

    LCD_ShowString(24, 174, 90, 12, 12, (uint8_t *)"KEY2 VOL+");

    POINT_COLOR = BLACK; BACK_COLOR = WHITE;

    LCD_ShowString(14, 204, 72, 12, 12, (uint8_t *)"BGM VOL");
    LCD_ShowString(14, 238, 48, 12, 12, (uint8_t *)"BEST");
    LCD_ShowxNum(58, 238, App_Settings_Get()->high_score, 5, 12, 0x80);

    draw_panel_box(140, 48, 228, 206, WHITE);
    POINT_COLOR = BLACK;
    LCD_ShowString(152, 60, 64, 12, 12, (uint8_t *)"GAME KEY");
    LCD_Fill(152, 76, 216, 77, LGRAY);
    POINT_COLOR = GRAY;
    LCD_ShowString(152, 94, 64, 12, 12, (uint8_t *)"K0 LEFT");
    LCD_ShowString(152, 114, 72, 12, 12, (uint8_t *)"K2 RIGHT");
    LCD_ShowString(152, 134, 64, 12, 12, (uint8_t *)"K1 ROT");
    LCD_ShowString(152, 154, 64, 12, 12, (uint8_t *)"UP DROP");
    LCD_ShowString(152, 184, 64, 12, 12, (uint8_t *)"UP REC");

    draw_button(14, 254, 124, 282, "PRESS KEY0", 1);
    draw_button(140, 254, 228, 282, "UP RECORD", 0);

    POINT_COLOR = LGRAY; BACK_COLOR = WHITE;

    menu_volume_drawn = 0xFF;
    menu_history_visible = 0;

}

/* 绘制 EEPROM 历史记录页，按最近记录分页显示分数和时间。 */
static void draw_eeprom_history_screen(void)
{
    const app_settings_t *s = App_Settings_Get();
    const uint8_t per_page = 2;
    uint8_t pages = (s->history_count + per_page - 1U) / per_page;
    uint8_t start;

    /* 小屏幕每页显示两条记录；每条记录分两行显示时间和分数。 */
    if (pages == 0U) {
        pages = 1U;
    }
    if (menu_history_page >= pages) {
        menu_history_page = (uint8_t)(pages - 1U);
    }
    start = (uint8_t)(menu_history_page * per_page);

    LCD_Fill(0, BOARD_Y_OFF, 239, 305, WHITE);
    LCD_Fill(0, 34, 239, 38, BLACK);
    POINT_COLOR = BLACK;
    BACK_COLOR = WHITE;

    LCD_ShowString(12, 48, 180, 16, 16, (uint8_t *)"EEPROM RECORD");
    LCD_Fill(12, 68, 226, 70, BLACK);
    draw_panel_box(12, 84, 226, 168, WHITE);

    POINT_COLOR = GRAY;
    LCD_ShowString(16, 92, 84, 12, 12, (uint8_t *)"BEST SCORE");
    LCD_ShowString(16, 122, 84, 12, 12, (uint8_t *)"BEST LINES");
    LCD_ShowString(16, 152, 84, 12, 12, (uint8_t *)"BGM VOLUME");

    POINT_COLOR = BLACK;
    LCD_ShowxNum(112, 92, s->high_score, 6, 12, 0x80);
    LCD_ShowxNum(112, 122, s->high_lines, 5, 12, 0x80);
    LCD_ShowxNum(112, 152, s->bgm_volume, 2, 12, 0x80);

    POINT_COLOR = BLACK;
    LCD_ShowString(16, 180, 64, 12, 12, (uint8_t *)"HISTORY");
    LCD_ShowxNum(94, 180, (uint32_t)menu_history_page + 1U, 1, 12, 0x80);
    LCD_ShowString(106, 180, 8, 12, 12, (uint8_t *)"/");
    LCD_ShowxNum(118, 180, pages, 1, 12, 0x80);

    POINT_COLOR = GRAY;
    if (s->history_count == 0U) {
        LCD_ShowString(16, 204, 144, 12, 12, (uint8_t *)"NO GAME RECORD");
    } else {
        for (uint8_t i = 0; i < per_page; i++) {
            app_game_history_t rec;
            uint8_t latest_index = (uint8_t)(start + i);
            uint16_t y = (uint16_t)(202 + i * 42);

            if (!App_Settings_GetHistory(latest_index, &rec)) {
                break;
            }

            LCD_Fill(14, y - 4U, 224, y - 3U, LGRAY);
            LCD_ShowString(16, y, 12, 12, 12, (uint8_t *)"#");
            LCD_ShowxNum(28, y, (uint32_t)latest_index + 1U, 2, 12, 0x80);
            LCD_ShowxNum(58, y, rec.started_at.month, 2, 12, 0x80);
            LCD_ShowString(82, y, 8, 12, 12, (uint8_t *)"/");
            LCD_ShowxNum(94, y, rec.started_at.day, 2, 12, 0x80);
            LCD_ShowxNum(130, y, rec.started_at.hour, 2, 12, 0x80);
            LCD_ShowString(154, y, 8, 12, 12, (uint8_t *)":");
            LCD_ShowxNum(166, y, rec.started_at.minute, 2, 12, 0x80);

            LCD_ShowString(28, y + 18, 12, 12, 12, (uint8_t *)"S");
            LCD_ShowxNum(40, y + 18, rec.score, 5, 12, 0x80);
            LCD_ShowString(100, y + 18, 12, 12, 12, (uint8_t *)"L");
            LCD_ShowxNum(112, y + 18, rec.lines, 3, 12, 0x80);
            LCD_ShowString(150, y + 18, 12, 12, 12, (uint8_t *)"T");
            LCD_ShowxNum(162, y + 18, rec.duration_seconds, 4, 12, 0x80);
        }
    }

    POINT_COLOR = LGRAY;
    LCD_ShowString(16, 292, 208, 12, 12, (uint8_t *)"K1/K2 PAGE  UP BACK");
}

/* 根据当前音量绘制菜单中的 10 格音量条，并避免重复刷新。 */
static void draw_menu_volume(void)
{
    uint8_t volume = Audio_BGM_GetVolume();
    uint16_t x = 14;
    uint16_t y = 214;

    if (menu_volume_drawn == volume) {
        return;
    }

    menu_volume_drawn = volume;
    LCD_Fill(x, y, x + 112, y + 12, WHITE);

    for (uint8_t i = 0; i < 10; i++) {
        uint16_t bx = (uint16_t)(x + i * 11U);
        uint16_t color = (i < volume) ? GREEN : LGRAY;
        LCD_Fill(bx, y + 2U, bx + 8U, y + 10U, color);
        POINT_COLOR = (i < volume) ? dim_color(GREEN) : GRAY;
        LCD_DrawRectangle(bx, y + 2U, bx + 8U, y + 10U);
    }
}


/* ---- 影子方块，下落位置预览 -------------------------------------- */

/* 计算当前活动方块如果硬降到底后的 Y 坐标。 */
static int8_t calc_ghost_y(void)

{

    int8_t gy = piece_y;

    while (!check_hit(piece_x, gy + 1, piece_rot)) gy++;

    return gy;

}

/* 擦除旧影子方块，恢复其覆盖位置的棋盘内容。 */
static void erase_ghost(void)

{

    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++)

        if (cell_set(piece_type, piece_rot, i, j)) {

            int bx = piece_x + i, by = ghost_y + j;

            if (bx >= 0 && bx < BOARD_COLS && by >= 0 && by < BOARD_ROWS)

                draw_block(bx, by, board[by][bx]);

        }

}

/* 绘制影子方块边框，提示当前方块最终落点。 */
static void draw_ghost(void)

{

    ghost_y = calc_ghost_y();

    if (ghost_y == piece_y) return;

    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++)

        if (cell_set(piece_type, piece_rot, i, j)) {

            int bx = piece_x + i, by = ghost_y + j;

            if (bx >= 0 && bx < BOARD_COLS && by >= 0 && by < BOARD_ROWS) {

                uint16_t px1 = BOARD_X_OFF + bx * BLOCK_SIZE;

                uint16_t py1 = BOARD_Y_OFF + by * BLOCK_SIZE;

                uint16_t px2 = px1 + BLOCK_SIZE - 1, py2 = py1 + BLOCK_SIZE - 1;

                POINT_COLOR = COLOR_MAP[piece_type + 1];

                LCD_DrawRectangle(px1 + 2, py1 + 2, px2 - 2, py2 - 2);

            }

        }

}


/* ---- 新游戏初始化 ------------------------------------------------ */

/* 重置棋盘和分数，记录开局时间，启动 BGM 并进入游戏状态。 */
static void start_new_game(void)

{

    memset(board, 0, sizeof(board));

    score = 0; lines_total = 0; level = 0;

    fall_speed = FALL_SPEED_INIT; game_state = STATE_PLAYING;
    Audio_BGM_Start();
    Led_Feedback_SetState(LED_FEEDBACK_PLAYING);

    last_fall_tick = last_key_tick = HAL_GetTick();
    game_start_tick = last_fall_tick;
    /* 记录本局开始时刻，后续写入 EEPROM 历史。 */
    App_RTC_GetDateTime(&game_start_time);

    srand(HAL_GetTick()); next_type = rand() % 7; new_piece();

    draw_static_ui();
    draw_board_full(); draw_piece(); draw_ghost(); draw_score_panel();

}


/* ==================================================================
 *  对外接口
 * ================================================================== */

/* 初始化 Tetris 模块，显示开始菜单并进入菜单状态。 */
void tetris_init(void)
{
    draw_static_ui();
    BACK_COLOR = WHITE;
    beep_stop();
    draw_start_screen();
    draw_menu_volume();
    Audio_BGM_Start();
    Led_Feedback_SetState(LED_FEEDBACK_MENU);
    game_state = STATE_MENU;
}


/* Tetris 主状态机，根据当前状态调度菜单、游戏中和 Game Over 逻辑。 */
void tetris_loop(void)
{
    beep_task();

    if (game_state == STATE_MENU) {
        static uint8_t menu_lock;
        static uint8_t vol_down_lock;
        static uint8_t vol_up_lock;
        static uint8_t history_lock;

        if (!menu_history_visible) {
            draw_menu_volume();
        }

        if (HAL_GPIO_ReadPin(KEY_UP_GPIO_Port, KEY_UP_Pin) == GPIO_PIN_SET) {
            if (!history_lock) {
                history_lock = 1;
                menu_history_visible ^= 1U;
                if (menu_history_visible) {
                    menu_history_page = 0;
                    draw_eeprom_history_screen();
                } else {
                    draw_start_screen();
                    draw_menu_volume();
                }
            }
        } else history_lock = 0;

        if (HAL_GPIO_ReadPin(KEY_0_GPIO_Port, KEY_0_Pin) == GPIO_PIN_RESET) {
        if (!menu_lock) { menu_lock = 1; start_new_game(); }
        } else menu_lock = 0;

        if (menu_history_visible && HAL_GPIO_ReadPin(KEY_1_GPIO_Port, KEY_1_Pin) == GPIO_PIN_RESET) {
            if (!vol_down_lock) {
                vol_down_lock = 1;
                if (menu_history_page > 0U) {
                    menu_history_page--;
                    draw_eeprom_history_screen();
                }
            }
        } else if (!menu_history_visible && HAL_GPIO_ReadPin(KEY_1_GPIO_Port, KEY_1_Pin) == GPIO_PIN_RESET) {
            if (!vol_down_lock) {
                uint8_t volume = Audio_BGM_GetVolume();
                vol_down_lock = 1;
                if (volume > 0) {
                    Audio_BGM_SetVolume(volume - 1);
                    App_Settings_SetVolume(volume - 1);
                    draw_menu_volume();
                }
            }
        } else vol_down_lock = 0;

        if (menu_history_visible && HAL_GPIO_ReadPin(KEY_2_GPIO_Port, KEY_2_Pin) == GPIO_PIN_RESET) {
            if (!vol_up_lock) {
                uint8_t pages = (uint8_t)((App_Settings_Get()->history_count + 1U) / 2U);
                vol_up_lock = 1;
                if (pages == 0U) {
                    pages = 1U;
                }
                if ((uint8_t)(menu_history_page + 1U) < pages) {
                    menu_history_page++;
                    draw_eeprom_history_screen();
                }
            }
        } else if (!menu_history_visible && HAL_GPIO_ReadPin(KEY_2_GPIO_Port, KEY_2_Pin) == GPIO_PIN_RESET) {
            if (!vol_up_lock) {
                uint8_t volume = Audio_BGM_GetVolume();
                vol_up_lock = 1;
                if (volume < 10) {
                    Audio_BGM_SetVolume(volume + 1);
                    App_Settings_SetVolume(volume + 1);
                    draw_menu_volume();
                }
            }
        } else vol_up_lock = 0;
        /* 红外遥控 PLAY 键也可以开始游戏。 */
        { uint32_t code; if (ir_read_key(&code) && code == 0x00FF02FD) start_new_game(); }
        return;
    }

    if (game_state == STATE_GAME_OVER) {
        static uint8_t restart_lock;
        static uint8_t menu_back_lock;
        if (HAL_GPIO_ReadPin(KEY_0_GPIO_Port, KEY_0_Pin) == GPIO_PIN_RESET) {
            if (!restart_lock) { restart_lock = 1; start_new_game(); }
        } else restart_lock = 0;
        if (HAL_GPIO_ReadPin(KEY_UP_GPIO_Port, KEY_UP_Pin) == GPIO_PIN_SET) {
            if (!menu_back_lock) { menu_back_lock = 1; return_to_start_menu(); }
        } else menu_back_lock = 0;
        { uint32_t code; if (ir_read_key(&code) && code == 0x00FF02FD) start_new_game(); }
        return;
    }

    /* 1. 本机按键轮询。 */
    game_key_handler();

    /* 1b. 红外遥控按键轮询。 */
    {
        uint32_t code;
        if (ir_read_key(&code)) {
            ir_do_action(code);
        }
    }

    /* 2. 重力下落。 */
    uint32_t now = HAL_GetTick();
    if (now - last_fall_tick >= fall_speed) {
        last_fall_tick = now;

        if (check_hit(piece_x, piece_y + 1, piece_rot)) {
            merge_piece(); remove_lines(); draw_board_full(); new_piece();
            if (game_state == STATE_PLAYING) { draw_piece(); draw_ghost(); }
            else { show_game_over(); }
        } else {
            erase_ghost(); erase_piece(); piece_y++; draw_piece(); draw_ghost();
        }
    }
}
