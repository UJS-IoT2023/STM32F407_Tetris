#include "audio_bgm.h"
#include "korobeiniki_pcm.h"
#include <stddef.h>

#define ES8388_ADDR             0x10U
#define AUDIO_SAMPLE_RATE       KOROBEINIKI_PCM_SAMPLE_RATE
#define AUDIO_BUFFER_SAMPLES    1024U
#define AUDIO_VOLUME_MAX        10U
#define AUDIO_VOLUME_DEFAULT    5U
#define AUDIO_VOLUME_LIMIT_PCT  3U

#define ES_I2C_SCL_Pin          GPIO_PIN_8
#define ES_I2C_SCL_GPIO_Port    GPIOB
#define ES_I2C_SDA_Pin          GPIO_PIN_9
#define ES_I2C_SDA_GPIO_Port    GPIOB

I2S_HandleTypeDef hi2s_bgm;
DMA_HandleTypeDef hdma_i2s_bgm_tx;

static int16_t audio_buffer[AUDIO_BUFFER_SAMPLES];
static volatile uint8_t audio_running;
static uint32_t pcm_pos;
static uint8_t bgm_volume = AUDIO_VOLUME_DEFAULT;

/* ES8388 和 EEPROM 共用 PB8/PB9，因此这里用软件 I2C 只在初始化
 * 音频 Codec 时发送寄存器配置。
 */
static void delay_short(void)
{
    for (volatile uint32_t i = 0; i < 60; i++) {
        __NOP();
    }
}

static void es_i2c_sda_out(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = ES_I2C_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(ES_I2C_SDA_GPIO_Port, &GPIO_InitStruct);
}

static void es_i2c_sda_in(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = ES_I2C_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(ES_I2C_SDA_GPIO_Port, &GPIO_InitStruct);
}

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

static int16_t next_sample(void)
{
    int16_t sample = korobeiniki_pcm[pcm_pos++];

    if (pcm_pos >= KOROBEINIKI_PCM_SAMPLE_COUNT) {
        pcm_pos = 0;
    }

    return sample;
}

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

static void i2s_clock_init(void)
{
    RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

    /* 8 kHz 对板载小扬声器已经足够，同时能明显减小 PCM 数组体积。 */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_I2S;
    PeriphClkInitStruct.PLLI2S.PLLI2SN = 192;
    PeriphClkInitStruct.PLLI2S.PLLI2SR = 5;
    (void)HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct);
}

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

void Audio_BGM_Start(void)
{
    if (audio_running) {
        return;
    }

    pcm_pos = 0;
    audio_running = 1U;
}

void Audio_BGM_Stop(void)
{
    audio_running = 0U;
}

void Audio_BGM_Task(void)
{
}

void Audio_BGM_SetVolume(uint8_t volume)
{
    if (volume > AUDIO_VOLUME_MAX) {
        volume = AUDIO_VOLUME_MAX;
    }

    bgm_volume = volume;
}

uint8_t Audio_BGM_GetVolume(void)
{
    return bgm_volume;
}

void Audio_BGM_DMA_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_i2s_bgm_tx);
}

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

void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI2) {
        /* 重新填充 DMA 刚发送完的前半段缓冲区。 */
        fill_audio(&audio_buffer[0], AUDIO_BUFFER_SAMPLES / 2U);
    }
}

void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI2) {
        /* 重新填充后半段缓冲区，实现非阻塞连续循环播放。 */
        fill_audio(&audio_buffer[AUDIO_BUFFER_SAMPLES / 2U], AUDIO_BUFFER_SAMPLES / 2U);
    }
}
