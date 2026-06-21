/**
 * @file tetris.h
 * @brief 俄罗斯方块游戏模块的公开接口和屏幕布局参数。
 *
 * 本文件集中定义 2.8 寸 240x320 LCD 适配后的棋盘尺寸、面板位置
 * 和游戏速度参数，便于后续继续调整 UI 布局。
 */
#ifndef __TETRIS_H
#define __TETRIS_H

#include "main.h"
#include "lcd.h"

/* ---- 棋盘几何参数 ---- */
#define BOARD_COLS   10   /* 棋盘列数，标准俄罗斯方块为 10 列。 */
#define BOARD_ROWS   20   /* 棋盘行数，标准俄罗斯方块为 20 行。 */
#define BLOCK_SIZE   12   /* 每个方块单元的像素边长，小屏适配为 12 像素。 */
#define BOARD_X_OFF  8    /* 棋盘左上角 X 坐标偏移。 */
#define BOARD_Y_OFF  34   /* 棋盘左上角 Y 坐标偏移，避开顶部标题栏。 */

/* ---- 右侧信息面板布局 ---- */
#define PANEL_X      148  /* 右侧 HUD 面板的起始 X 坐标。 */
#define NEXT_Y       54   /* 下一方块预览区域的起始 Y 坐标。 */
#define NEXT_SIZE    12   /* 下一方块预览中每个小格子的像素边长。 */
#define SCORE_Y      128  /* 分数数值显示 Y 坐标。 */
#define LINES_Y      170  /* 消除行数数值显示 Y 坐标。 */
#define LEVEL_Y      212  /* 等级数值显示 Y 坐标。 */
#define CTRL_Y       264  /* 底部按键说明区域起始 Y 坐标。 */

/* ---- 游戏时间参数，单位 ms ---- */
#define KEY_DEBOUNCE  20   /* 按键消抖间隔，避免一次按下触发多次。 */
#define FALL_SPEED_INIT 500/* 初始自动下落间隔。 */
#define FALL_SPEED_MIN  100/* 等级提升后允许的最小下落间隔。 */

/* ---- 方块编码说明 ----
 * 7 种方块 x 4 种旋转状态，每个旋转状态用 16 位表示 4x4 位图。
 * 第 j 行、第 i 列对应 bit(j*4+i)，为 1 表示该位置有方块。
 */
extern const uint16_t shapes[7][4];

/* 方块颜色表：索引 0 表示空格，1..7 对应七种方块颜色。 */
extern const uint16_t COLOR_MAP[8];

/* 初始化游戏界面、菜单状态、BGM 和 LED 菜单反馈。 */
void tetris_init(void);
/* 游戏主循环入口，负责菜单、游戏中、Game Over 三种状态调度。 */
void tetris_loop(void);

#endif /* __TETRIS_H */
