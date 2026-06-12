#ifndef __TETRIS_H
#define __TETRIS_H

#include "main.h"
#include "lcd.h"

/* ---- Board geometry ---- */
#define BOARD_COLS   10
#define BOARD_ROWS   20
#define BLOCK_SIZE   32
#define BOARD_X_OFF  20
#define BOARD_Y_OFF  40

/* ---- Right-side panel layout ---- */
#define PANEL_X      360
#define NEXT_Y       80
#define NEXT_SIZE    20          /* small block size for next-piece preview */
#define SCORE_Y      220
#define LINES_Y      280
#define LEVEL_Y      340
#define CTRL_Y       520

/* ---- Game timing (milliseconds) ---- */
#define KEY_DEBOUNCE  20
#define FALL_SPEED_INIT 500
#define FALL_SPEED_MIN  100

/* ---- Shape encoding ----
 * 7 tetrominoes x 4 rotations, each a 16-bit bitmap.
 * Row j (0=top), column i (0=left):  bit  j*4+i
 */
extern const uint16_t shapes[7][4];

/* ---- Colour map: index 0 = empty, 1..7 = piece colours ---- */
extern const uint16_t COLOR_MAP[8];

/* ---- Public API ---- */
void tetris_init(void);
void tetris_loop(void);

#endif /* __TETRIS_H */