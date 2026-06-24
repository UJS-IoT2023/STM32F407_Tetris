/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : STM32F407 Tetris 程序入口和应用模块调度。
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* 头文件 --------------------------------------------------------------------*/
#include "main.h"
#include "tim.h"
#include "gpio.h"
#include "fsmc.h"

/* 私有头文件 ----------------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "lcd.h"
#include "tetris.h"
#include "audio_bgm.h"
#include "app_settings.h"
#include "app_rtc.h"
#include "led_feedback.h"
#include "spi_flash.h"
/* USER CODE END Includes */

/* 私有类型定义 --------------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* 私有宏定义 ----------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* 私有宏 --------------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* 私有变量 ------------------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* 私有函数声明 --------------------------------------------------------------*/
void SystemClock_Config(void); /* 配置 HSI+PLL 系统时钟，供 HAL 和各外设使用。 */
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* 用户代码 ------------------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  主函数入口，完成 HAL、外设和应用模块初始化后进入轮询主循环。
  * @retval int 理论返回值；嵌入式主循环不会正常返回。
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU 基础配置 ------------------------------------------------------------*/

  /* 复位外设并初始化 Flash 接口和 SysTick。 */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* 配置系统时钟。 */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* 初始化 CubeMX 配置的外设。 */
  MX_GPIO_Init();
  MX_FSMC_Init();
  MX_TIM3_Init();
  MX_TIM1_Init();
  /* USER CODE BEGIN 2 */
  HAL_TIM_Base_Start(&htim3); /* 启动 TIM3 基准计时，供相关外设/逻辑使用。 */
  /* PA8 红外接收头由 TIM1 输入捕获采样 NEC 波形。 */
  /* TIM1_CH1 用于红外 NEC 协议输入捕获，更新中断用于超时判定。 */
  HAL_TIM_IC_Start_IT(&htim1, TIM_CHANNEL_1);
  __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_UPDATE);
  /* 项目功能模块初始化顺序：
   * LED/SPI FLASH/音频先准备硬件，RTC 提供时间，EEPROM 再读取设置。
   */
  Led_Feedback_Init();  /* 初始化 LED0/LED1 游戏状态反馈。 */
  SpiFlash_Init();      /* 初始化外部 SPI FLASH 软件 SPI 和芯片 ID。 */
  Audio_BGM_Init();     /* 初始化 ES8388、I2S2 和 DMA 循环播放缓冲。 */
  App_RTC_Init();       /* 初始化 RTC，为历史记录提供开局时间。 */
  App_Settings_Init();  /* 读取 EEPROM 中的最高分、音量和历史记录。 */
  Audio_BGM_SetVolume(App_Settings_Get()->bgm_volume); /* 应用 EEPROM 中保存的音量。 */
  /* Tetris 初始化会绘制开始菜单，并按 EEPROM 中的音量设置播放 BGM。 */
  tetris_init();
  /* USER CODE END 2 */

  /* 主循环。 */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* 三个任务都设计为非阻塞轮询，避免影响按键响应和游戏下落节奏。 */
    Audio_BGM_Task();    /* BGM 预留任务入口，当前播放主要由 DMA 回调维护。 */
    Led_Feedback_Task(); /* 刷新 LED 非阻塞闪烁状态机。 */
    tetris_loop();       /* 执行 Tetris 菜单、游戏和结算状态机。 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief 配置系统时钟为 HSI 经 PLL 倍频后的高速时钟。
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0}; /* 振荡器和 PLL 配置结构体。 */
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0}; /* SYSCLK/HCLK/PCLK 总线分频配置结构体。 */

  /** 配置内部主稳压器输出电压。
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** 按 RCC_OscInitTypeDef 参数初始化 RCC 振荡器。
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** 初始化 CPU、AHB 和 APB 总线时钟。
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  HAL 初始化或时钟配置失败时进入的错误处理函数。
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* 发生 HAL 错误时停在这里，便于调试定位。 */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* 断言失败时可在这里输出文件名和行号。 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
