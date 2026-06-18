#include "tetris.h"
#include <stdlib.h>
#include <string.h>

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

/* colour index: 0=empty, 1=I … 7=O */
const uint16_t COLOR_MAP[8] = {
    WHITE,   /* 0 background */
    CYAN,    /* 1 I */
    MAGENTA, /* 2 T */
    GREEN,   /* 3 S */
    RED,     /* 4 Z */
    BLUE,    /* 5 J */
    BRRED,   /* 6 L  (brown-red ≈ orange) */
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
static uint8_t  game_over;

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
    LCD_ShowxNum(PANEL_X, SCORE_Y, score,       6, 16, 0x80);
    LCD_ShowxNum(PANEL_X, LINES_Y, lines_total,  6, 16, 0x80);
    LCD_ShowxNum(PANEL_X, LEVEL_Y, level,         2, 16, 0x80);
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
        game_over = 1;
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

/* ---- key handler (non-blocking, 20 ms debounce) ------------------ */

static void game_key_handler(void)
{
    uint32_t now = HAL_GetTick();
    if (now - last_key_tick < KEY_DEBOUNCE) return;
    last_key_tick = now;

    static uint8_t k0_lock, k1_lock, k2_lock, kup_lock;

    /* KEY0 (PE4, active-low) → move left */
    if (HAL_GPIO_ReadPin(KEY_2_GPIO_Port, KEY_2_Pin) == GPIO_PIN_RESET) {
        if (!k0_lock) {
            k0_lock = 1;
            if (!check_hit(piece_x - 1, piece_y, piece_rot)) {
                erase_piece();
                piece_x--;
                draw_piece();
            }
        }
    } else k0_lock = 0;

    /* KEY2 (PE2, active-low) → move right */
    if (HAL_GPIO_ReadPin(KEY_0_GPIO_Port, KEY_0_Pin) == GPIO_PIN_RESET) {
        if (!k2_lock) {
            k2_lock = 1;
            if (!check_hit(piece_x + 1, piece_y, piece_rot)) {
                erase_piece();
                piece_x++;
                draw_piece();
            }
        }
    } else k2_lock = 0;

    /* KEY1 (PE3, active-low) → rotate */
    if (HAL_GPIO_ReadPin(KEY_UP_GPIO_Port, KEY_UP_Pin) == GPIO_PIN_RESET) {
        if (!k1_lock) {
            k1_lock = 1;

            /* Erase the OLD position first (uses current piece_rot) */
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
        }
    } else k1_lock = 0;

    /* WK_UP (PA0, active-high) → hard drop */
    if (HAL_GPIO_ReadPin(KEY_1_GPIO_Port, KEY_1_Pin) == GPIO_PIN_SET) {
        if (!kup_lock) {
            kup_lock = 1;
            erase_piece();
            while (!check_hit(piece_x, piece_y + 1, piece_rot))
                piece_y++;
            merge_piece();
            draw_board_full();
            remove_lines();
            draw_board_full();     /* redraw after line shift */
            beep_short();
            new_piece();
            if (!game_over) draw_piece();
        }
    } else kup_lock = 0;
}

/* ---- game-over screen ------------------------------------------- */

static void show_game_over(void)
{
    /* dark overlay on the board area */
    LCD_Fill(BOARD_X_OFF, BOARD_Y_OFF,
             BOARD_X_OFF + BOARD_COLS * BLOCK_SIZE - 1,
             BOARD_Y_OFF + BOARD_ROWS * BLOCK_SIZE - 1, BLACK);
    POINT_COLOR = RED;
    BACK_COLOR  = BLACK;
    LCD_ShowString(BOARD_X_OFF + 10, 300, 300, 24, 24,
                   (uint8_t *)"GAME  OVER");
    POINT_COLOR = WHITE;
    LCD_ShowString(BOARD_X_OFF + 10, 340, 300, 16, 16,
                   (uint8_t *)"Press KEY0");
}

/* ---- static UI (Swiss-style) ------------------------------------ */

static void draw_static_ui(void)
{
    LCD_Init();
    LCD_Display_Dir(0);   /* portrait 480 x 800 */
    LCD_Clear(WHITE);

    /* header */
    POINT_COLOR = BLACK;
    LCD_ShowString(20, 10, 200, 24, 24, (uint8_t *)"T E T R I S");

    /* thick divider line */
    LCD_Fill(20, 35, 460, 38, BLACK);

    /* board border */
    LCD_DrawRectangle(BOARD_X_OFF - 1, BOARD_Y_OFF - 1,
                      BOARD_X_OFF + BOARD_COLS * BLOCK_SIZE,
                      BOARD_Y_OFF + BOARD_ROWS * BLOCK_SIZE);

    /* right panel labels */
    POINT_COLOR = GRAY;
    LCD_ShowString(PANEL_X, NEXT_Y - 20, 80, 16, 16, (uint8_t *)"NEXT");
    LCD_DrawRectangle(PANEL_X - 1, NEXT_Y - 1,
                      PANEL_X + 4 * NEXT_SIZE,
                      NEXT_Y  + 4 * NEXT_SIZE);

    POINT_COLOR = BLACK;
    LCD_ShowString(PANEL_X, SCORE_Y - 20, 80, 16, 16, (uint8_t *)"SCORE");
    LCD_ShowString(PANEL_X, LINES_Y - 20, 80, 16, 16, (uint8_t *)"LINES");
    LCD_ShowString(PANEL_X, LEVEL_Y - 20, 80, 16, 16, (uint8_t *)"LEVEL");

    POINT_COLOR = LGRAY;
    LCD_ShowString(PANEL_X, CTRL_Y,      100, 12, 12, (uint8_t *)"K0:  LEFT");
    LCD_ShowString(PANEL_X, CTRL_Y + 16, 100, 12, 12, (uint8_t *)"K2:  RIGHT");
    LCD_ShowString(PANEL_X, CTRL_Y + 32, 100, 12, 12, (uint8_t *)"K1:  ROTATE");
    LCD_ShowString(PANEL_X, CTRL_Y + 48, 100, 12, 12, (uint8_t *)"KUP: DROP");

    /* footer divider */
    LCD_Fill(20, 760, 460, 763, BLACK);
    POINT_COLOR = LGRAY;
    LCD_ShowString(20, 770, 300, 12, 12, (uint8_t *)"arorms.cn  STM32F407");
}

/* ==================================================================
 *  Public API
 * ================================================================== */

void tetris_init(void)
{
    draw_static_ui();

    memset(board, 0, sizeof(board));
    score       = 0;
    lines_total = 0;
    level       = 0;
    fall_speed  = FALL_SPEED_INIT;
    game_over   = 0;
    last_fall_tick = HAL_GetTick();
    last_key_tick  = HAL_GetTick();

    srand(HAL_GetTick());
    next_type = rand() % 7;
    new_piece();

    draw_board_full();
    draw_piece();
    draw_score_panel();
}

void tetris_loop(void)
{
    if (game_over) {
        /* restart on KEY0 press */
        static uint8_t restart_lock;
        if (HAL_GPIO_ReadPin(KEY_0_GPIO_Port, KEY_0_Pin) == GPIO_PIN_RESET) {
            if (!restart_lock) {
                restart_lock = 1;
                tetris_init();
            }
        } else restart_lock = 0;
        return;
    }

    /* 1. key polling */
    game_key_handler();

    /* 2. gravity */
    uint32_t now = HAL_GetTick();
    if (now - last_fall_tick >= fall_speed) {
        last_fall_tick = now;

        if (check_hit(piece_x, piece_y + 1, piece_rot)) {
            /* piece has landed */
            merge_piece();
            remove_lines();
            draw_board_full();
            new_piece();
            if (!game_over) {
                draw_piece();
            } else {
                show_game_over();
            }
        } else {
            erase_piece();
            piece_y++;
            draw_piece();
        }
    }
}