#include "tetris.h"
#include "audio_bgm.h"
#include <stdlib.h>
#include <string.h>
/* IR decoder externs (from stm32f4xx_it.c) */
extern volatile uint8_t  ir_ready;
extern volatile uint32_t ir_code;

/* ===================================================================
 *  Shape data  (tinytetris-compatible 4x4 bitmaps)
 *  Row j (0=top of 4x4 grid), col i (0=left):  bit  j*4 + i
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

/* colour index: 0=empty, 1=I 鈥?7=O */
const uint16_t COLOR_MAP[8] = {
    WHITE,   /* 0 background */
    CYAN,    /* 1 I */
    MAGENTA, /* 2 T */
    GREEN,   /* 3 S */
    RED,     /* 4 Z */
    BLUE,    /* 5 J */
    BRRED,   /* 6 L  (brown-red 鈮?orange) */
    YELLOW,  /* 7 O */
};

/* ---- game state -------------------------------------------------- */
static uint8_t  board[BOARD_ROWS][BOARD_COLS];   /* 0=empty, 1-7=colour */
static int8_t   piece_type, next_type;
static int8_t   piece_rot;                        /* 0-3 */
static int8_t   piece_x, piece_y;                 /* grid coords (top-left of 4x4) */
static uint32_t score, lines_total, level;
static uint32_t fall_speed;                       /* ms per gravity tick */
static uint32_t last_fall_tick, last_key_tick;
static uint8_t  game_state;
static int8_t   ghost_y;
static uint8_t  menu_volume_drawn = 0xFF;

#define STATE_MENU       0
#define STATE_PLAYING    1
#define STATE_GAME_OVER  2

/* ---- helpers ----------------------------------------------------- */
static inline uint16_t shape_word(int type, int rot)
{
    return shapes[type][rot];
}

static inline int cell_set(int type, int rot, int i, int j)
{
    return (shape_word(type, rot) >> (j * 4 + i)) & 1;
}

/* ---- low-level drawing ------------------------------------------- */

/* Draw a single block at grid (gx, gy) with colour index ci */
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
        /* highlight: top & left edges */
        POINT_COLOR = WHITE;
        LCD_DrawLine(px1, py1, px2, py1);
        LCD_DrawLine(px1, py1, px1, py2);
        /* shadow: bottom & right edges */
        POINT_COLOR = GRAY;
        LCD_DrawLine(px1, py2, px2, py2);
        LCD_DrawLine(px2, py1, px2, py2);
    }
}

/* Draw one small block for the next-piece preview panel */
static void draw_next_block(uint8_t gx, uint8_t gy, uint8_t ci)
{
    uint16_t px1 = PANEL_X + gx * NEXT_SIZE;
    uint16_t py1 = NEXT_Y  + gy * NEXT_SIZE;
    uint16_t px2 = px1 + NEXT_SIZE - 1;
    uint16_t py2 = py1 + NEXT_SIZE - 1;
    LCD_Fill(px1, py1, px2, py2, ci ? COLOR_MAP[ci] : WHITE);
}

/* ---- board / piece rendering ------------------------------------- */

static void draw_board_full(void)
{
    for (int r = 0; r < BOARD_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++)
            draw_block(c, r, board[r][c]);
}

/* Erase current piece from screen (restore board cells underneath) */
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

/* Draw current piece on screen */
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

/* Draw the next-piece preview box */
static void draw_next_piece(void)
{
    /* clear preview area */
    LCD_Fill(PANEL_X, NEXT_Y,
             PANEL_X + 4 * NEXT_SIZE - 1,
             NEXT_Y  + 4 * NEXT_SIZE - 1, WHITE);
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            if (cell_set(next_type, 0, i, j))
                draw_next_block(i, j, next_type + 1);
}

/* ---- score / level display --------------------------------------- */

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

/* ---- collision detection ----------------------------------------- */

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

/* ---- merge piece into board -------------------------------------- */

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

/* ---- line removal ----------------------------------------------- */

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
            /* shift rows above down */
            for (int rr = r; rr > 0; rr--)
                memcpy(board[rr], board[rr - 1], BOARD_COLS);
            memset(board[0], 0, BOARD_COLS);
            r++; /* recheck this row */
        }
    }
    if (cleared) {
        static const int pts[] = { 0, 100, 300, 500, 800 };
        score       += pts[cleared];
        lines_total += cleared;
        level        = lines_total / 10;
        fall_speed   = FALL_SPEED_INIT - level * 30;
        if (fall_speed < FALL_SPEED_MIN) fall_speed = FALL_SPEED_MIN;
        draw_score_panel();
    }
}

/* ---- spawn new piece --------------------------------------------- */

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

/* ---- beep feedback ---------------------------------------------- */

static void beep_short(void)
{
    HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_SET);
    HAL_Delay(20);
    HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_RESET);
}

/* forward decl for ghost functions used in key handler */
static void erase_ghost(void);
static void draw_ghost(void);

/* ---- key handler (non-blocking, 20 ms debounce) ------------------ */

static void game_key_handler(void)
{
    uint32_t now = HAL_GetTick();
    if (now - last_key_tick < KEY_DEBOUNCE) return;
    last_key_tick = now;

    static uint8_t k0_lock, k1_lock, k2_lock, kup_lock;

    /* KEY0 (PE4, active-low) 鈫?move left */
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

    /* KEY2 (PE2, active-low) 鈫?move right */
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

    /* KEY1 (PE3, active-low) 鈫?rotate */
    if (HAL_GPIO_ReadPin(KEY_UP_GPIO_Port, KEY_UP_Pin) == GPIO_PIN_RESET) {
        if (!k1_lock) {
            k1_lock = 1;

            /* Erase the OLD position first (uses current piece_rot) */
            erase_ghost();
            erase_piece();

            int new_rot = (piece_rot + 1) & 3;
            int new_x   = piece_x;

            /* wall-kick left: if new rotation's right edge overflows */
            while (new_x + rightmost_col(piece_type, new_rot) >= BOARD_COLS)
                new_x--;

            /* try kicking right by 1 or 2 cells if still colliding */
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
                /* if not ok: leave piece_rot/piece_x unchanged (rollback) */
            } else {
                piece_rot = new_rot;
                piece_x   = new_x;
            }

            draw_piece();
            draw_ghost();
        }
    } else k1_lock = 0;

   /* WK_UP (PA0, active-high) 鈫?hard drop */
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
            draw_board_full();     /* redraw after line shift */
            beep_short();
            new_piece();
            if (game_state == STATE_PLAYING) { draw_piece(); draw_ghost(); }
        }
    } else kup_lock = 0;
}

/* ---- game-over screen ------------------------------------------- */

static void show_game_over(void)
{
    Audio_BGM_Stop();
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
    POINT_COLOR = GRAY;
    LCD_ShowString(BOARD_X_OFF + 20, BOARD_Y_OFF + 172, 96, 12, 12,
                   (uint8_t *)"KEY0 START");
}

/* ---- static UI (Swiss-style) ------------------------------------ */

static void draw_static_ui(void)
{
    LCD_Init();
    LCD_Display_Dir(0);   /* portrait 240 x 320 */
    LCD_Clear(WHITE);
    BACK_COLOR  = WHITE;

    /* header */
    POINT_COLOR = BLACK;
    LCD_ShowString(8, 8, 140, 16, 16, (uint8_t *)"TETRIS");

    /* thick divider line */
    LCD_Fill(8, 27, 232, 29, BLACK);

    /* board border */
    LCD_DrawRectangle(BOARD_X_OFF - 1, BOARD_Y_OFF - 1,
                      BOARD_X_OFF + BOARD_COLS * BLOCK_SIZE,
                      BOARD_Y_OFF + BOARD_ROWS * BLOCK_SIZE);

    /* right panel labels */
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

    /* footer divider */
    LCD_Fill(8, 306, 232, 307, BLACK);
    POINT_COLOR = LGRAY;
    LCD_ShowString(8, 309, 144, 12, 12, (uint8_t *)"STM32F407");
}
/* forward declarations for ghost functions used by ir_do_action */

static void erase_ghost(void);

static void draw_ghost(void);


/* ---- IR remote key polling -------------------------------------- */

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


/* ---- start screen ----------------------------------------------- */

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


/* ---- ghost piece (drop preview) --------------------------------- */

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


/* ---- game init helpers ------------------------------------------ */

static void start_new_game(void)

{

    memset(board, 0, sizeof(board));

    score = 0; lines_total = 0; level = 0;

    fall_speed = FALL_SPEED_INIT; game_state = STATE_PLAYING;
    Audio_BGM_Start();

    last_fall_tick = last_key_tick = HAL_GetTick();

    srand(HAL_GetTick()); next_type = rand() % 7; new_piece();

    draw_static_ui();
    draw_board_full(); draw_piece(); draw_ghost(); draw_score_panel();

}


/* ==================================================================
 *  Public API
 * ================================================================== */

void tetris_init(void)
{
    draw_static_ui();
    BACK_COLOR = WHITE;
    draw_start_screen();
    draw_menu_volume();
    Audio_BGM_Start();
    game_state = STATE_MENU;
}


void tetris_loop(void)
{
    if (game_state == STATE_MENU) {
        static uint8_t menu_lock;
        static uint8_t vol_down_lock;
        static uint8_t vol_up_lock;
        draw_menu_volume();
        if (HAL_GPIO_ReadPin(KEY_0_GPIO_Port, KEY_0_Pin) == GPIO_PIN_RESET) {
        if (!menu_lock) { menu_lock = 1; start_new_game(); }
        } else menu_lock = 0;
        if (HAL_GPIO_ReadPin(KEY_1_GPIO_Port, KEY_1_Pin) == GPIO_PIN_RESET) {
            if (!vol_down_lock) {
                uint8_t volume = Audio_BGM_GetVolume();
                vol_down_lock = 1;
                if (volume > 0) {
                    Audio_BGM_SetVolume(volume - 1);
                    draw_menu_volume();
                }
            }
        } else vol_down_lock = 0;
        if (HAL_GPIO_ReadPin(KEY_2_GPIO_Port, KEY_2_Pin) == GPIO_PIN_RESET) {
            if (!vol_up_lock) {
                uint8_t volume = Audio_BGM_GetVolume();
                vol_up_lock = 1;
                if (volume < 10) {
                    Audio_BGM_SetVolume(volume + 1);
                    draw_menu_volume();
                }
            }
        } else vol_up_lock = 0;
        /* IR remote start (PLAY button) */
        { uint32_t code; if (ir_read_key(&code) && code == 0x00FF02FD) start_new_game(); }
        return;
    }

    if (game_state == STATE_GAME_OVER) {
        static uint8_t restart_lock;
        if (HAL_GPIO_ReadPin(KEY_0_GPIO_Port, KEY_0_Pin) == GPIO_PIN_RESET) {
            if (!restart_lock) { restart_lock = 1; start_new_game(); }
        } else restart_lock = 0;
        { uint32_t code; if (ir_read_key(&code) && code == 0x00FF02FD) start_new_game(); }
        return;
    }

    /* 1. key polling */
    game_key_handler();

    /* 1b. IR remote key polling */
    {
        uint32_t code;
        if (ir_read_key(&code)) {
            ir_do_action(code);
        }
    }

    /* 2. gravity */
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
