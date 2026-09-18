/**
 * recorder.cpp
 *
 * Após iniciar → aguarda 20s → grava 60s → envia WAV pelo serial (115200)
 */

#include <Arduino.h>
#include "driver/i2s.h"

#define I2S_PORT        I2S_NUM_0
#define I2S_BCK         26
#define I2S_WS          25
#define I2S_DATA        33

#define SAMPLE_RATE     16000
#define RECORD_SECONDS  60
#define TOTAL_SAMPLES   (SAMPLE_RATE * RECORD_SECONDS)
#define I2S_BUF_SIZE    512

#define LED_STATUS      19

#define START_DELAY_MS  20000

#define CHUNK_SAMPLES   256

int32_t i2s_raw[I2S_BUF_SIZE];
int16_t chunk_buf[CHUNK_SAMPLES];

void init_i2s() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = I2S_BUF_SIZE,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0
    };

    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_BCK,
        .ws_io_num    = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = I2S_DATA
    };

    i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer(I2S_PORT);
}

void send_wav_header(uint32_t num_samples) {
    uint32_t data_bytes  = num_samples * 2;
    uint32_t riff_size   = 36 + data_bytes;
    uint16_t audio_fmt   = 1;
    uint16_t channels    = 1;
    uint32_t sample_rate = SAMPLE_RATE;
    uint32_t byte_rate   = SAMPLE_RATE * 2;
    uint16_t block_align = 2;
    uint16_t bps         = 16;
    uint32_t fmt_size    = 16;

    Serial.write((uint8_t*)"RIFF", 4);
    Serial.write((uint8_t*)&riff_size, 4);
    Serial.write((uint8_t*)"WAVE", 4);
    Serial.write((uint8_t*)"fmt ", 4);
    Serial.write((uint8_t*)&fmt_size, 4);
    Serial.write((uint8_t*)&audio_fmt, 2);
    Serial.write((uint8_t*)&channels, 2);
    Serial.write((uint8_t*)&sample_rate, 4);
    Serial.write((uint8_t*)&byte_rate, 4);
    Serial.write((uint8_t*)&block_align, 2);
    Serial.write((uint8_t*)&bps, 2);
    Serial.write((uint8_t*)"data", 4);
    Serial.write((uint8_t*)&data_bytes, 4);

    Serial.flush();
}

void record_and_send() {
    // Pequena pausa para o Python estar pronto
    delay(500);

    send_wav_header(TOTAL_SAMPLES);

    uint32_t collected = 0;
    int chunk_pos = 0;

    while (collected < TOTAL_SAMPLES) {
        size_t bytes_read = 0;

        i2s_read(
            I2S_PORT,
            i2s_raw,
            sizeof(i2s_raw),
            &bytes_read,
            portMAX_DELAY
        );

        int n = bytes_read / sizeof(int32_t);

        for (int i = 0; i < n && collected < TOTAL_SAMPLES; i++) {
            int32_t s = i2s_raw[i] >> 11;

            chunk_buf[chunk_pos++] =
                (int16_t)constrain(s, -32768, 32767);

            collected++;

            if (chunk_pos == CHUNK_SAMPLES ||
                collected == TOTAL_SAMPLES) {

                Serial.write(
                    (uint8_t*)chunk_buf,
                    chunk_pos * 2
                );

                Serial.flush();

                chunk_pos = 0;
            }
        }
    }

    // Marcador de fim
    delay(200);

    Serial.print("\n##REC_END##\n");
    Serial.flush();
}

void setup() {
    Serial.begin(115200);
    delay(1500);

    pinMode(LED_STATUS, OUTPUT);
    digitalWrite(LED_STATUS, LOW);

    init_i2s();

    // Avisa que o ESP32 está pronto
    delay(1000);
    Serial.print("##READY##\n");
    Serial.flush();

    // Aguarda 20 segundos antes de começar
    delay(START_DELAY_MS);

    // Avisa o Python que a gravação vai começar
    digitalWrite(LED_STATUS, HIGH);

    Serial.print("##REC_START##\n");
    Serial.flush();

    // Grava e envia os 60 segundos
    record_and_send();

    // Finaliza
    digitalWrite(LED_STATUS, LOW);
}

void loop() {
}