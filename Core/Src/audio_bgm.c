/**
 * @file audio_bgm.c
 * @brief Korobeiniki 背景音乐播放实现。
 *
 * 本模块使用 ES8388 Codec 输出音频，I2S2 负责发送 PCM 数据，
 * DMA 循环模式负责持续搬运缓冲区。MP3 已在离线阶段转成 8 kHz
 * 单声道 PCM C 数组，运行时不再进行 MP3 解码。
 */
#include "audio_bgm.h"
#include "korobeiniki_pcm.h"
#include <stddef.h>

#define ES8388_ADDR             0x10U /* ES8388 7 位 I2C 地址。 */
#define AUDIO_SAMPLE_RATE       KOROBEINIKI_PCM_SAMPLE_RATE /* PCM 源采样率。 */
#define AUDIO_BUFFER_SAMPLES    1024U /* I2S DMA 循环缓冲区采样点数，包含左右声道。 */
#define AUDIO_VOLUME_MAX        10U   /* 菜单中允许的最大音量档位。 */
#define AUDIO_VOLUME_DEFAULT    5U    /* EEPROM 无设置时的默认音量档位。 */
#define AUDIO_VOLUME_LIMIT_PCT  3U    /* 最大输出限制为原 PCM 的 3%，保护小扬声器。 */

#define ES_I2C_SCL_Pin          GPIO_PIN_8 /* ES8388 软件 I2C 时钟引脚。 */
#define ES_I2C_SCL_GPIO_Port    GPIOB      /* ES8388 SCL 所在端口。 */
#define ES_I2C_SDA_Pin          GPIO_PIN_9 /* ES8388 软件 I2C 数据引脚。 */
#define ES_I2C_SDA_GPIO_Port    GPIOB      /* ES8388 SDA 所在端口。 */

I2S_HandleTypeDef hi2s_bgm;          /* BGM 专用 I2S2 句柄。 */
DMA_HandleTypeDef hdma_i2s_bgm_tx;   /* I2S2 TX 对应的 DMA1 Stream4 句柄。 */

static int16_t audio_buffer[AUDIO_BUFFER_SAMPLES]; /* DMA 循环发送的双声道 PCM 缓冲区。 */
static volatile uint8_t audio_running;             /* 播放状态，0 输出静音，1 输出音乐。 */
static uint32_t pcm_pos;                            /* 当前读取到 korobeiniki_pcm 的位置。 */
static uint8_t bgm_volume = AUDIO_VOLUME_DEFAULT;   /* 当前用户音量档位。 */

/* ES8388 和 EEPROM 共用 PB8/PB9，因此这里用软件 I2C 只在初始化
 * 音频 Codec 时发送寄存器配置。
 */
/* 软件 I2C 位时序短延时。 */
static void delay_short(void)
{
    for (volatile uint32_t i = 0; i < 60; i++) {
        __NOP();
    }
}

/* 将 ES8388 软件 I2C 的 SDA 切换为开漏输出。 */
static void es_i2c_sda_out(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = ES_I2C_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(ES_I2C_SDA_GPIO_Port, &GPIO_InitStruct);
}

/* 将 ES8388 软件 I2C 的 SDA 切换为输入，用于读取 ACK。 */
static void es_i2c_sda_in(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = ES_I2C_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(ES_I2C_SDA_GPIO_Port, &GPIO_InitStruct);
}

/* 初始化 ES8388 软件 I2C 引脚，并释放总线。 */
static void es_i2c_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitStruct.Pin = ES_I2C_SCL_Pin | ES_I2C_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    HAL_GPIO_WritePin(ES_I2C_SCL_GPIO_Port, ES_I2C_SCL_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(ES_I2C_SDA_GPIO_Port, ES_I2C_SDA_Pin, GPIO_PIN_SET);
}

/* 产生软件 I2C 起始条件。 */
static void es_i2c_start(void)
{
    es_i2c_sda_out();
    HAL_GPIO_WritePin(ES_I2C_SDA_GPIO_Port, ES_I2C_SDA_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(ES_I2C_SCL_GPIO_Port, ES_I2C_SCL_Pin, GPIO_PIN_SET);
    delay_short();
    HAL_GPIO_WritePin(ES_I2C_SDA_GPIO_Port, ES_I2C_SDA_Pin, GPIO_PIN_RESET);
    delay_short();
    HAL_GPIO_WritePin(ES_I2C_SCL_GPIO_Port, ES_I2C_SCL_Pin, GPIO_PIN_RESET);
}

/* 产生软件 I2C 停止条件。 */
static void es_i2c_stop(void)
{
    es_i2c_sda_out();
    HAL_GPIO_WritePin(ES_I2C_SCL_GPIO_Port, ES_I2C_SCL_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(ES_I2C_SDA_GPIO_Port, ES_I2C_SDA_Pin, GPIO_PIN_RESET);
    delay_short();
    HAL_GPIO_WritePin(ES_I2C_SCL_GPIO_Port, ES_I2C_SCL_Pin, GPIO_PIN_SET);
    delay_short();
    HAL_GPIO_WritePin(ES_I2C_SDA_GPIO_Port, ES_I2C_SDA_Pin, GPIO_PIN_SET);
    delay_short();
}

/* 等待 ES8388 ACK，应答低电平返回 1。 */
static uint8_t es_i2c_wait_ack(void)
{
    uint8_t ack;

    es_i2c_sda_in();
    HAL_GPIO_WritePin(ES_I2C_SCL_GPIO_Port, ES_I2C_SCL_Pin, GPIO_PIN_SET);
    delay_short();
    ack = (HAL_GPIO_ReadPin(ES_I2C_SDA_GPIO_Port, ES_I2C_SDA_Pin) == GPIO_PIN_RESET);
    HAL_GPIO_WritePin(ES_I2C_SCL_GPIO_Port, ES_I2C_SCL_Pin, GPIO_PIN_RESET);
    es_i2c_sda_out();
    return ack;
}

/* 通过软件 I2C 发送 1 字节，并返回是否收到 ACK。 */
static uint8_t es_i2c_send_byte(uint8_t data)
{
    es_i2c_sda_out();
    for (uint8_t i = 0; i < 8; i++) {
        HAL_GPIO_WritePin(ES_I2C_SDA_GPIO_Port, ES_I2C_SDA_Pin,
                          (data & 0x80U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        data <<= 1;
        delay_short();
        HAL_GPIO_WritePin(ES_I2C_SCL_GPIO_Port, ES_I2C_SCL_Pin, GPIO_PIN_SET);
        delay_short();
        HAL_GPIO_WritePin(ES_I2C_SCL_GPIO_Port, ES_I2C_SCL_Pin, GPIO_PIN_RESET);
    }
    return es_i2c_wait_ack();
}

/* 写 ES8388 单个寄存器，返回 0 表示成功，1 表示通信失败。 */
static uint8_t es8388_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t ok;

    es_i2c_start();
    ok = es_i2c_send_byte((ES8388_ADDR << 1) | 0U);
    ok &= es_i2c_send_byte(reg);
    ok &= es_i2c_send_byte(val);
    es_i2c_stop();

    return ok ? 0U : 1U;
}

/* 设置 ES8388 模拟输出音量，四个输出通道保持同一值。 */
static void es8388_set_volume(uint8_t volume)
{
    if (volume > 33U) {
        volume = 33U;
    }

    es8388_write_reg(0x2E, volume);
    es8388_write_reg(0x2F, volume);
    es8388_write_reg(0x30, volume);
    es8388_write_reg(0x31, volume);
}

/* 配置 ES8388 为播放模式，打开 DAC 和输出通路。 */
static void es8388_init_playback(void)
{
    es_i2c_init();

    /* 只配置播放所需的 Codec 通路：电源、DAC、I2S 格式和较低的
     * 模拟输出音量；菜单音量通过 PCM 数字缩放完成。
     */
    es8388_write_reg(0x00, 0x80);
    es8388_write_reg(0x00, 0x00);
    HAL_Delay(100);

    es8388_write_reg(0x01, 0x58);
    es8388_write_reg(0x01, 0x50);
    es8388_write_reg(0x02, 0xF3);
    es8388_write_reg(0x02, 0xF0);
    es8388_write_reg(0x03, 0x09);
    es8388_write_reg(0x00, 0x06);
    es8388_write_reg(0x04, 0x00);
    es8388_write_reg(0x08, 0x00);
    es8388_write_reg(0x2B, 0x80);
    es8388_write_reg(0x17, 0x18);
    es8388_write_reg(0x18, 0x02);
    es8388_write_reg(0x1A, 0x00);
    es8388_write_reg(0x1B, 0x00);
    es8388_write_reg(0x27, 0xB8);
    es8388_write_reg(0x2A, 0xB8);
    es8388_write_reg(0x02, 0x0A);
    es8388_write_reg(0x04, 0x3C);
    es8388_set_volume(8);
}

/* 从 PCM 数组取下一个单声道采样，读到末尾后回到开头循环播放。 */
static int16_t next_sample(void)
{
    int16_t sample = korobeiniki_pcm[pcm_pos++];

    if (pcm_pos >= KOROBEINIKI_PCM_SAMPLE_COUNT) {
        pcm_pos = 0;
    }

    return sample;
}

/* 填充 I2S DMA 缓冲区；count 为 int16_t 元素数，按左右声道成对写入。 */
static void fill_audio(int16_t *dst, size_t count)
{
    /* DMA 缓冲区按双声道输出，BGM 源数据为单声道，所以每个采样
     * 同时写入左右声道，并按 0..10 的菜单音量做缩放。
     */
    for (size_t i = 0; i < count; i += 2U) {
        int16_t sample = audio_running ? next_sample() : 0;
        sample = (int16_t)(((int32_t)sample * bgm_volume * AUDIO_VOLUME_LIMIT_PCT) /
                           (AUDIO_VOLUME_MAX * 100));
        dst[i] = sample;
        dst[i + 1U] = sample;
    }
}

/* 配置 I2S 专用 PLL，使 SPI2/I2S2 能输出 8 kHz 音频。 */
static void i2s_clock_init(void)
{
    RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

    /* 8 kHz 对板载小扬声器已经足够，同时能明显减小 PCM 数组体积。 */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_I2S;
    PeriphClkInitStruct.PLLI2S.PLLI2SN = 192;
    PeriphClkInitStruct.PLLI2S.PLLI2SR = 5;
    (void)HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct);
}

/* 初始化音频硬件和 DMA 循环缓冲，启动后默认静音。 */
void Audio_BGM_Init(void)
{
    i2s_clock_init();
    es8388_init_playback();

    /* DMA 只启动一次，之后在回调中持续填充静音或音乐数据。 */
    fill_audio(audio_buffer, AUDIO_BUFFER_SAMPLES);

    hi2s_bgm.Instance = SPI2;
    hi2s_bgm.Init.Mode = I2S_MODE_MASTER_TX;
    hi2s_bgm.Init.Standard = I2S_STANDARD_PHILIPS;
    hi2s_bgm.Init.DataFormat = I2S_DATAFORMAT_16B;
    hi2s_bgm.Init.MCLKOutput = I2S_MCLKOUTPUT_ENABLE;
    hi2s_bgm.Init.AudioFreq = I2S_AUDIOFREQ_8K;
    hi2s_bgm.Init.CPOL = I2S_CPOL_LOW;
    hi2s_bgm.Init.ClockSource = I2S_CLOCK_PLL;
    hi2s_bgm.Init.FullDuplexMode = I2S_FULLDUPLEXMODE_DISABLE;

    if (HAL_I2S_Init(&hi2s_bgm) != HAL_OK) {
        return;
    }

    (void)HAL_I2S_Transmit_DMA(&hi2s_bgm, (uint16_t *)audio_buffer, AUDIO_BUFFER_SAMPLES);
    Audio_BGM_Stop();
}

/* 从 PCM 开头开始播放 BGM。 */
void Audio_BGM_Start(void)
{
    if (audio_running) {
        return;
    }

    pcm_pos = 0;
    audio_running = 1U;
}

/* 停止播放音乐，DMA 回调继续填充静音以保持 I2S 时钟稳定。 */
void Audio_BGM_Stop(void)
{
    audio_running = 0U;
}

/* 预留周期任务入口，目前 DMA 回调已经完成全部播放维护。 */
void Audio_BGM_Task(void)
{
}

/* 设置菜单音量档位，并限制在 0..AUDIO_VOLUME_MAX。 */
void Audio_BGM_SetVolume(uint8_t volume)
{
    if (volume > AUDIO_VOLUME_MAX) {
        volume = AUDIO_VOLUME_MAX;
    }

    bgm_volume = volume;
}

/* 返回当前菜单音量档位。 */
uint8_t Audio_BGM_GetVolume(void)
{
    return bgm_volume;
}

/* 给 stm32f4xx_it.c 调用的 DMA 中断包装函数。 */
void Audio_BGM_DMA_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_i2s_bgm_tx);
}

/* HAL I2S 底层初始化回调，配置 SPI2/I2S2 引脚和 TX DMA。 */
void HAL_I2S_MspInit(I2S_HandleTypeDef *hi2s)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (hi2s->Instance != SPI2) {
        return;
    }

    __HAL_RCC_SPI2_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    GPIO_InitStruct.Pin = GPIO_PIN_12 | GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_3 | GPIO_PIN_6;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    hdma_i2s_bgm_tx.Instance = DMA1_Stream4;
    hdma_i2s_bgm_tx.Init.Channel = DMA_CHANNEL_0;
    hdma_i2s_bgm_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_i2s_bgm_tx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_i2s_bgm_tx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_i2s_bgm_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_i2s_bgm_tx.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_i2s_bgm_tx.Init.Mode = DMA_CIRCULAR;
    hdma_i2s_bgm_tx.Init.Priority = DMA_PRIORITY_HIGH;
    hdma_i2s_bgm_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;

    HAL_DMA_DeInit(&hdma_i2s_bgm_tx);
    if (HAL_DMA_Init(&hdma_i2s_bgm_tx) != HAL_OK) {
        return;
    }

    __HAL_LINKDMA(hi2s, hdmatx, hdma_i2s_bgm_tx);
    HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);
}

/* I2S DMA 前半缓冲发送完成回调，立即补充前半缓冲。 */
void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI2) {
        /* 重新填充 DMA 刚发送完的前半段缓冲区。 */
        fill_audio(&audio_buffer[0], AUDIO_BUFFER_SAMPLES / 2U);
    }
}

/* I2S DMA 后半缓冲发送完成回调，立即补充后半缓冲。 */
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI2) {
        /* 重新填充后半段缓冲区，实现非阻塞连续循环播放。 */
        fill_audio(&audio_buffer[AUDIO_BUFFER_SAMPLES / 2U], AUDIO_BUFFER_SAMPLES / 2U);
    }
}
