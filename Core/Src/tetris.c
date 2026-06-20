#include "tetris.h"
#include "audio_bgm.h"
#include "app_rtc.h"
#include "app_settings.h"
#include "led_feedback.h"
#include "spi_flash.h"
#include <stdlib.h>
#include <string.h>
/* 红外解码结果由 stm32f4xx_it.c 的中断逻辑更新。 */
extern volatile uint8_t  ir_ready;
extern volatile uint32_t ir_code;

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
static uint8_t  board[BOARD_ROWS][BOARD_COLS];   /* 0 为空格，1..7 为颜色索引 */
static int8_t   piece_type, next_type;
static int8_t   piece_rot;                        /* 当前旋转形态，范围 0..3 */
static int8_t   piece_x, piece_y;                 /* 4x4 方块左上角在棋盘中的坐标 */
static uint32_t score, lines_total, level;
static uint32_t fall_speed;                       /* 自动下落间隔，单位 ms */
static uint32_t last_fall_tick, last_key_tick;
static uint8_t  game_state;
static int8_t   ghost_y;
static uint8_t  menu_volume_drawn = 0xFF;
static uint8_t  menu_history_visible;
static uint8_t  menu_history_page;
static uint32_t game_start_tick;
static app_datetime_t game_start_time;

#define STATE_MENU       0
#define STATE_PLAYING    1
#define STATE_GAME_OVER  2

/* ---- 通用辅助函数 ------------------------------------------------ */
static inline uint16_t shape_word(int type, int rot)
{
    return shapes[type][rot];
}

static inline int cell_set(int type, int rot, int i, int j)
{
    return (shape_word(type, rot) >> (j * 4 + i)) & 1;
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
        POINT_COLOR = LGRAY;
        LCD_DrawRectangle(px1, py1, px2, py2);
    } else {
        LCD_Fill(px1, py1, px2, py2, COLOR_MAP[ci]);
        /* 左上高光，增强小方块立体感。 */
        POINT_COLOR = WHITE;
        LCD_DrawLine(px1, py1, px2, py1);
        LCD_DrawLine(px1, py1, px1, py2);
        /* 右下阴影。 */
        POINT_COLOR = GRAY;
        LCD_DrawLine(px1, py2, px2, py2);
        LCD_DrawLine(px2, py1, px2, py2);
    }
}

/* 绘制右侧 NEXT 预览框中的小方块。 */
static void draw_next_block(uint8_t gx, uint8_t gy, uint8_t ci)
{
    uint16_t px1 = PANEL_X + gx * NEXT_SIZE;
    uint16_t py1 = NEXT_Y  + gy * NEXT_SIZE;
    uint16_t px2 = px1 + NEXT_SIZE - 1;
    uint16_t py2 = py1 + NEXT_SIZE - 1;
    LCD_Fill(px1, py1, px2, py2, ci ? COLOR_MAP[ci] : WHITE);
}

/* ---- 棋盘和方块绘制 ---------------------------------------------- */

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

static int rightmost_col(int type, int rot)
{
    for (int c = 3; c >= 0; c--)
        for (int r = 0; r < 4; r++)
            if (cell_set(type, rot, c, r))
                return c;
    return 0;
}

static void remove_lines(void)
{
    int cleared = 0;
    for (int r = BOARD_ROWS - 1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < BOARD_COLS; c++)
            if (!board[r][c]) { full = 0; break; }
        if (full) {
            cleared++;
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
        draw_score_panel();
    }
}

/* ---- 生成新方块 -------------------------------------------------- */

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

/* ---- 按键音反馈 -------------------------------------------------- */

static void beep_short(void)
{
    HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_SET);
    HAL_Delay(20);
    HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_RESET);
}

/* 影子方块函数声明，按键处理中需要提前使用。 */
static void erase_ghost(void);
static void draw_ghost(void);

/* ---- 按键处理，非阻塞，20 ms 消抖 ------------------------------- */

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
            beep_short();
            new_piece();
            if (game_state == STATE_PLAYING) { draw_piece(); draw_ghost(); }
        }
    } else kup_lock = 0;
}

/* ---- 游戏结束页面 ------------------------------------------------ */

static void draw_start_screen(void);
static void draw_menu_volume(void);

static void show_game_over(void)
{
    spi_flash_game_record_t last_record;
    uint32_t duration_seconds = (HAL_GetTick() - game_start_tick) / 1000U;

    Audio_BGM_Stop();
    Led_Feedback_SetState(LED_FEEDBACK_GAME_OVER);
    /* 游戏结束时同时写 EEPROM 历史记录和 SPI FLASH 最近一局快照。 */
    App_Settings_AddGameResult(score, lines_total, duration_seconds, &game_start_time);
    last_record.score = score;
    last_record.lines = lines_total;
    last_record.level = level;
    last_record.volume = Audio_BGM_GetVolume();
    (void)SpiFlash_SaveGameRecord(&last_record);
    LCD_Fill(BOARD_X_OFF + 4, BOARD_Y_OFF + 52,
             BOARD_X_OFF + BOARD_COLS * BLOCK_SIZE - 5,
             BOARD_Y_OFF + 184, WHITE);
    POINT_COLOR = BLACK;
    BACK_COLOR  = WHITE;
    LCD_ShowString(BOARD_X_OFF + 10, BOARD_Y_OFF + 62, 120, 24, 24,
                   (uint8_t *)"GAME");
    LCD_ShowString(BOARD_X_OFF + 10, BOARD_Y_OFF + 92, 120, 24, 24,
                   (uint8_t *)"OVER");
    LCD_ShowString(BOARD_X_OFF + 12, BOARD_Y_OFF + 132, 72, 12, 12, (uint8_t *)"SCORE");
    LCD_ShowxNum(BOARD_X_OFF + 54, BOARD_Y_OFF + 132, score, 5, 12, 0x80);
    LCD_ShowString(BOARD_X_OFF + 12, BOARD_Y_OFF + 150, 72, 12, 12, (uint8_t *)"LINES");
    LCD_ShowxNum(BOARD_X_OFF + 54, BOARD_Y_OFF + 150, lines_total, 4, 12, 0x80);
    LCD_ShowString(BOARD_X_OFF + 12, BOARD_Y_OFF + 168, 72, 12, 12, (uint8_t *)"BEST");
    LCD_ShowxNum(BOARD_X_OFF + 54, BOARD_Y_OFF + 168, App_Settings_Get()->high_score, 5, 12, 0x80);
    POINT_COLOR = GRAY;
    LCD_ShowString(BOARD_X_OFF + 12, BOARD_Y_OFF + 188, 96, 12, 12,
                   (uint8_t *)"KEY0 START");
    LCD_ShowString(BOARD_X_OFF + 12, BOARD_Y_OFF + 204, 96, 12, 12,
                   (uint8_t *)"UP MENU");
}

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

static void draw_static_ui(void)
{
    LCD_Init();
    LCD_Display_Dir(0);   /* 竖屏 240 x 320。 */
    LCD_Clear(WHITE);
    BACK_COLOR  = WHITE;

    /* 顶部标题。 */
    POINT_COLOR = BLACK;
    LCD_ShowString(8, 8, 140, 16, 16, (uint8_t *)"TETRIS");

    /* 顶部分隔线。 */
    LCD_Fill(8, 27, 232, 29, BLACK);

    /* 棋盘边框。 */
    LCD_DrawRectangle(BOARD_X_OFF - 1, BOARD_Y_OFF - 1,
                      BOARD_X_OFF + BOARD_COLS * BLOCK_SIZE,
                      BOARD_Y_OFF + BOARD_ROWS * BLOCK_SIZE);

    /* 右侧信息面板。 */
    POINT_COLOR = GRAY;
    LCD_ShowString(PANEL_X, NEXT_Y - 16, 56, 12, 12, (uint8_t *)"NEXT");
    LCD_DrawRectangle(PANEL_X - 1, NEXT_Y - 1,
                      PANEL_X + 4 * NEXT_SIZE,
                      NEXT_Y  + 4 * NEXT_SIZE);

    POINT_COLOR = BLACK;
    LCD_ShowString(PANEL_X, SCORE_Y - 14, 56, 12, 12, (uint8_t *)"SCORE");
    LCD_ShowString(PANEL_X, LINES_Y - 14, 56, 12, 12, (uint8_t *)"LINES");
    LCD_ShowString(PANEL_X, LEVEL_Y - 14, 56, 12, 12, (uint8_t *)"LEVEL");

    POINT_COLOR = LGRAY;
    LCD_ShowString(8, CTRL_Y,      88, 12, 12, (uint8_t *)"K0 LEFT");
    LCD_ShowString(8, CTRL_Y + 14, 88, 12, 12, (uint8_t *)"K2 RIGHT");
    LCD_ShowString(8, CTRL_Y + 28, 88, 12, 12, (uint8_t *)"K1 ROT");
    LCD_ShowString(8, CTRL_Y + 42, 88, 12, 12, (uint8_t *)"UP DROP");

    /* 底部分隔线。 */
    LCD_Fill(8, 306, 232, 307, BLACK);
    POINT_COLOR = LGRAY;
    LCD_ShowString(8, 309, 144, 12, 12, (uint8_t *)"STM32F407");
}
/* 影子方块函数声明，红外遥控动作中也会使用。 */

static void erase_ghost(void);

static void draw_ghost(void);


/* ---- 红外遥控按键轮询 -------------------------------------------- */

static uint8_t ir_read_key(uint32_t *code)

{

    uint8_t ret = 0;

    __disable_irq();

    if (ir_ready) { ir_ready = 0; *code = ir_code; ret = 1; }

    __enable_irq();

    return ret;

}

static void ir_do_action(uint32_t code)

{

    switch (code) {

    case 0x00FF22DD:

        if (!check_hit(piece_x - 1, piece_y, piece_rot))

            { erase_ghost(); erase_piece(); piece_x--; draw_piece(); draw_ghost(); } break;

    case 0x00FFC23D:

        if (!check_hit(piece_x + 1, piece_y, piece_rot))

            { erase_ghost(); erase_piece(); piece_x++; draw_piece(); draw_ghost(); } break;

    case 0x00FF629D:

        { erase_ghost(); erase_piece();

          int nr = (piece_rot + 1) & 3, nx = piece_x;

          while (nx + rightmost_col(piece_type, nr) >= BOARD_COLS) nx--;

          if (check_hit(nx, piece_y, nr)) {

              for (int k = 1; k <= 2; k++)

                  if (!check_hit(nx+k, piece_y, nr)) { nx += k; break; }

              if (check_hit(nx, piece_y, nr)) { nr = piece_rot; nx = piece_x; }

          }

          piece_rot = nr; piece_x = nx; draw_piece(); draw_ghost();

        } break;

    case 0x00FF02FD:

        { erase_ghost(); erase_piece();

          while (!check_hit(piece_x, piece_y + 1, piece_rot)) piece_y++;

          merge_piece(); draw_board_full(); remove_lines(); draw_board_full(); beep_short();

          new_piece();

          if (game_state == STATE_PLAYING) { draw_piece(); draw_ghost(); }

          else { show_game_over(); }

        } break;

    }

}


/* ---- 开始菜单 ---------------------------------------------------- */

static void draw_start_screen(void)

{

    static const uint16_t bar_c[] = { CYAN, MAGENTA, GREEN, YELLOW, RED, BLUE };

    LCD_Fill(0, BOARD_Y_OFF, 239, 305, WHITE);

    for (int i = 0; i < 6; i++) {
        LCD_Fill(12 + i * 18, 48, 26 + i * 18, 62, bar_c[i]);
    }

    POINT_COLOR = BLACK; BACK_COLOR = WHITE;

    LCD_ShowString(14, 82, 112, 24, 24, (uint8_t *)"TETRIS");

    LCD_Fill(14, 112, 122, 114, BLACK);

    POINT_COLOR = LGRAY;

    LCD_ShowString(14, 132, 100, 12, 12, (uint8_t *)"KEY0 START");

    LCD_ShowString(14, 150, 100, 12, 12, (uint8_t *)"KEY1 VOL-");

    LCD_ShowString(14, 168, 100, 12, 12, (uint8_t *)"KEY2 VOL+");

    POINT_COLOR = BLACK; BACK_COLOR = WHITE;

    LCD_ShowString(14, 198, 72, 12, 12, (uint8_t *)"BGM VOL");
    LCD_ShowString(14, 232, 48, 12, 12, (uint8_t *)"BEST");
    LCD_ShowxNum(58, 232, App_Settings_Get()->high_score, 5, 12, 0x80);

    POINT_COLOR = GRAY;
    LCD_DrawRectangle(142, 48, 226, 206);
    LCD_ShowString(152, 60, 64, 12, 12, (uint8_t *)"GAME KEY");
    LCD_Fill(152, 76, 216, 77, LGRAY);
    LCD_ShowString(152, 96, 64, 12, 12, (uint8_t *)"K0 LEFT");
    LCD_ShowString(152, 116, 72, 12, 12, (uint8_t *)"K2 RIGHT");
    LCD_ShowString(152, 136, 64, 12, 12, (uint8_t *)"K1 ROT");
    LCD_ShowString(152, 156, 64, 12, 12, (uint8_t *)"UP DROP");

    POINT_COLOR = WHITE; BACK_COLOR = BLACK;

    LCD_Fill(14, 248, 122, 278, BLACK);

    LCD_ShowString(28, 257, 96, 12, 12, (uint8_t *)"PRESS KEY0");

    POINT_COLOR = LGRAY; BACK_COLOR = WHITE;

    menu_volume_drawn = 0xFF;
    menu_history_visible = 0;

}

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
    POINT_COLOR = BLACK;
    BACK_COLOR = WHITE;

    LCD_ShowString(12, 48, 180, 16, 16, (uint8_t *)"EEPROM RECORD");
    LCD_Fill(12, 68, 226, 70, BLACK);

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

static void draw_menu_volume(void)
{
    uint8_t volume = Audio_BGM_GetVolume();
    uint16_t x = 14;
    uint16_t y = 214;

    if (menu_volume_drawn == volume) {
        return;
    }

    menu_volume_drawn = volume;
    LCD_Fill(x, y, x + 106, y + 8, WHITE);

    for (uint8_t i = 0; i < 10; i++) {
        uint16_t color = (i < volume) ? GREEN : LGRAY;
        LCD_Fill(x + i * 10, y, x + i * 10 + 7, y + 7, color);
    }
}


/* ---- 影子方块，下落位置预览 -------------------------------------- */

static int8_t calc_ghost_y(void)

{

    int8_t gy = piece_y;

    while (!check_hit(piece_x, gy + 1, piece_rot)) gy++;

    return gy;

}

static void erase_ghost(void)

{

    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++)

        if (cell_set(piece_type, piece_rot, i, j)) {

            int bx = piece_x + i, by = ghost_y + j;

            if (bx >= 0 && bx < BOARD_COLS && by >= 0 && by < BOARD_ROWS)

                draw_block(bx, by, board[by][bx]);

        }

}

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

void tetris_init(void)
{
    draw_static_ui();
    BACK_COLOR = WHITE;
    draw_start_screen();
    draw_menu_volume();
    Audio_BGM_Start();
    Led_Feedback_SetState(LED_FEEDBACK_MENU);
    game_state = STATE_MENU;
}


void tetris_loop(void)
{
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
