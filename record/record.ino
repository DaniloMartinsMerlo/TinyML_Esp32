#include <Arduino.h>
#include "driver/i2s.h"
#include "Kabum_inferencing.h"
#include <math.h>

// ============================================================
// CONFIGURAÇÃO DO I2S
// ============================================================

#define I2S_PORT        I2S_NUM_0
#define I2S_BCK         26
#define I2S_WS          25
#define I2S_DATA        33

#define SAMPLE_RATE     EI_CLASSIFIER_FREQUENCY
#define WINDOW_SAMPLES  EI_CLASSIFIER_RAW_SAMPLE_COUNT
#define I2S_BUFFER_SIZE 512

// ============================================================
// PINOS DE SAÍDA
// ============================================================

#define LED_VERMELHO 18
#define LED_VERDE    19
#define RELE         22

// Módulo de relê active-LOW (mais comum nos labs):
// LOW  = relê LIGA
// HIGH = relê DESLIGA
#define RELE_ON  LOW
#define RELE_OFF HIGH

#define CONFIDENCE_THRESHOLD 0.70f

// ============================================================
// HANDLES FREERTOS
// ============================================================

TaskHandle_t taskHandleAudioCapture;
TaskHandle_t taskHandleFeatureExtraction;
TaskHandle_t taskHandleAnomalyDetection;

SemaphoreHandle_t audioBufferSemaphore;
QueueHandle_t     featureQueue;

// ============================================================
// BUFFERS DE ÁUDIO (double buffering)
// ============================================================

int32_t i2s_buffer[I2S_BUFFER_SIZE];

int16_t audio_buffer_a[WINDOW_SAMPLES];
int16_t audio_buffer_b[WINDOW_SAMPLES];

int16_t* capture_buffer    = audio_buffer_a;
int16_t* processing_buffer = audio_buffer_b;

volatile bool buffer_a_ready = false;
volatile bool buffer_b_ready = false;

portMUX_TYPE bufferMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================
// ESTRUTURA DE FEATURES
// ============================================================

struct AudioFeatures {
    float    rms;
    float    spectral_centroid;
    uint32_t feature_time_us;
    int16_t* audio_data;
};

// ============================================================
// CALLBACK DO EDGE IMPULSE
// Converte int16 → float normalizado para o classificador
// ============================================================

static int get_signal_data(size_t offset, size_t length, float* out_ptr) {
    for (size_t i = 0; i < length; i++) {
        out_ptr[i] = (float)processing_buffer[offset + i] / 32768.0f;
    }
    return 0;
}

// ============================================================
// INICIALIZAÇÃO DO I2S
// ============================================================

bool init_i2s() {
    Serial.println("Inicializando I2S...");

    i2s_config_t i2s_config = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = I2S_BUFFER_SIZE,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num   = I2S_BCK,
        .ws_io_num    = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = I2S_DATA
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("ERRO i2s_driver_install: %d\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("ERRO i2s_set_pin: %d\n", err);
        return false;
    }

    i2s_zero_dma_buffer(I2S_PORT);
    Serial.println("I2S OK");
    return true;
}

// ============================================================
// CAPTURA DE ÁUDIO
// Lê samples do I2S até preencher WINDOW_SAMPLES
// ============================================================

bool capture_audio(int16_t* buffer) {
    size_t   collected  = 0;
    uint32_t start_time = micros();

    while (collected < WINDOW_SAMPLES) {
        size_t    bytes_read = 0;
        esp_err_t err        = i2s_read(
            I2S_PORT,
            i2s_buffer,
            sizeof(i2s_buffer),
            &bytes_read,
            portMAX_DELAY
        );

        if (err != ESP_OK) {
            Serial.printf("ERRO i2s_read: %d\n", err);
            return false;
        }

        int samples_read = bytes_read / sizeof(int32_t);

        for (int i = 0; i < samples_read && collected < WINDOW_SAMPLES; i++) {
            // INMP441: dado de 24-bit chegando em slot de 32-bit, shift >> 14
            // para converter para int16 com saturação
            int32_t sample = i2s_buffer[i] >> 14;
            sample = constrain(sample, -32768, 32767);
            buffer[collected++] = (int16_t)sample;
        }
    }

    Serial.printf("[TASK 1] %d samples em %lu us\n", WINDOW_SAMPLES, micros() - start_time);
    return true;
}

// ============================================================
// RMS — energia do sinal
// ============================================================

float calculate_rms(int16_t* buffer) {
    double sum = 0.0;
    for (size_t i = 0; i < WINDOW_SAMPLES; i++) {
        float sample = (float)buffer[i] / 32768.0f;
        sum += sample * sample;
    }
    return sqrtf((float)(sum / WINDOW_SAMPLES));
}

// ============================================================
// SPECTRAL CENTROID — "centro de massa" do espectro
// Aproximação no domínio do tempo (sem FFT completa)
// ============================================================

float calculate_spectral_centroid(int16_t* buffer) {
    double weighted_sum  = 0.0;
    double magnitude_sum = 0.0;

    for (size_t i = 1; i < WINDOW_SAMPLES; i++) {
        float magnitude  = fabsf((float)buffer[i]);
        weighted_sum    += (double)i * magnitude;
        magnitude_sum   += magnitude;
    }

    if (magnitude_sum < 1e-9) return 0.0f;

    float centroid_index = (float)(weighted_sum / magnitude_sum);
    return centroid_index * ((float)SAMPLE_RATE / WINDOW_SAMPLES);
}

// ============================================================
// TASK 1 — CAPTURA DE ÁUDIO (prioridade 5, Core 1)
//
// Preenche alternadamente audio_buffer_a e audio_buffer_b.
// Sinaliza a Task 2 via semáforo binário a cada buffer cheio.
// ============================================================

void task_audio_capture(void* parameter) {
    Serial.println("[TASK 1] Audio Capture iniciada | Core 1 | Prioridade 5");

    while (true) {
        int16_t* target = capture_buffer;
        bool success    = capture_audio(target);

        if (!success) {
            Serial.println("[TASK 1] Erro na captura, tentando novamente...");
            continue;
        }

        portENTER_CRITICAL(&bufferMux);
        if (target == audio_buffer_a) buffer_a_ready = true;
        else                          buffer_b_ready = true;
        portEXIT_CRITICAL(&bufferMux);

        // Alterna o buffer de captura
        capture_buffer = (capture_buffer == audio_buffer_a)
                         ? audio_buffer_b
                         : audio_buffer_a;

        // Sinaliza que há buffer pronto para a Task 2
        xSemaphoreGive(audioBufferSemaphore);

        taskYIELD();
    }
}

// ============================================================
// TASK 2 — EXTRAÇÃO DE FEATURES (prioridade 3, Core 0)
//
// Aguarda semáforo da Task 1, calcula RMS e Spectral Centroid,
// e envia AudioFeatures (com ponteiro para o buffer de áudio)
// para a featureQueue consumida pela Task 3.
//
// Nota: MFCC é calculado internamente pelo Edge Impulse SDK
// durante run_classifier() na Task 3.
// ============================================================

void task_feature_extraction(void* parameter) {
    Serial.println("[TASK 2] Feature Extraction iniciada | Core 0 | Prioridade 3");

    while (true) {
        if (xSemaphoreTake(audioBufferSemaphore, portMAX_DELAY) != pdTRUE) continue;

        int16_t* buf_to_process = nullptr;

        portENTER_CRITICAL(&bufferMux);
        if (buffer_a_ready) {
            buf_to_process = audio_buffer_a;
            buffer_a_ready = false;
        } else if (buffer_b_ready) {
            buf_to_process = audio_buffer_b;
            buffer_b_ready = false;
        }
        portEXIT_CRITICAL(&bufferMux);

        if (buf_to_process == nullptr) continue;

        processing_buffer = buf_to_process;

        uint32_t start_time = micros();

        AudioFeatures features;
        features.rms               = calculate_rms(buf_to_process);
        features.spectral_centroid = calculate_spectral_centroid(buf_to_process);
        features.feature_time_us   = micros() - start_time;
        features.audio_data        = buf_to_process;

        Serial.printf(
            "[TASK 2] RMS: %.4f | Centroid: %.1f Hz | %lu us\n",
            features.rms,
            features.spectral_centroid,
            features.feature_time_us
        );

        if (xQueueSend(featureQueue, &features, portMAX_DELAY) != pdTRUE) {
            Serial.println("[TASK 2] ERRO ao enviar para featureQueue");
        }
    }
}

// ============================================================
// TASK 3 — DETECÇÃO DE ANOMALIA (prioridade 1, Core 0)
//
// Consome AudioFeatures da fila, roda o classificador Edge
// Impulse e aciona os atuadores conforme a classe detectada:
//   "Kabum"    → Liga o relê
//   "Verde"    → Acende LED verde
//   "Vermelho" → Acende LED vermelho
//   noise/unknown → sem ação
// ============================================================

void task_anomaly_detection(void* parameter) {
    Serial.println("[TASK 3] Anomaly Detection iniciada | Core 0 | Prioridade 1");

    AudioFeatures features;

    while (true) {
        if (xQueueReceive(featureQueue, &features, portMAX_DELAY) != pdTRUE) continue;

        processing_buffer = features.audio_data;

        uint32_t start_time = micros();

        signal_t signal;
        signal.total_length = WINDOW_SAMPLES;
        signal.get_data     = &get_signal_data;

        ei_impulse_result_t result;
        EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);

        uint32_t inference_time = micros() - start_time;

        if (err != EI_IMPULSE_OK) {
            Serial.printf("[TASK 3] Erro na inferencia: %d\n", err);
            continue;
        }

        // Coleta scores por nome (robusto a mudanças de ordem no modelo)
        float kabum_score    = 0.0f;
        float verde_score    = 0.0f;
        float vermelho_score = 0.0f;

        Serial.println("\n========== RESULTADO ==========");
        for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
            const char* label = result.classification[i].label;
            float       value = result.classification[i].value;
            Serial.printf("  %-10s : %.5f\n", label, value);

            if (strcmp(label, "Kabum")    == 0) kabum_score    = value;
            if (strcmp(label, "Verde")    == 0) verde_score    = value;
            if (strcmp(label, "Vermelho") == 0) vermelho_score = value;
        }
        Serial.printf("  Inferencia    : %lu us\n", inference_time);
        Serial.printf("  Feature extr. : %lu us\n", features.feature_time_us);

        // Desliga tudo antes de decidir (evita estados sobrepostos)
        digitalWrite(LED_VERDE,    LOW);
        digitalWrite(LED_VERMELHO, LOW);
        digitalWrite(RELE,         RELE_OFF);

        // Aciona o atuador da classe vencedora acima do threshold
        if (kabum_score >= CONFIDENCE_THRESHOLD) {
            Serial.println(">>> KABUM DETECTADO! — Relê LIGADO");
            digitalWrite(RELE, RELE_ON);

        } else if (verde_score >= CONFIDENCE_THRESHOLD) {
            Serial.println(">>> VERDE DETECTADO! — LED verde LIGADO");
            digitalWrite(LED_VERDE, HIGH);

        } else if (vermelho_score >= CONFIDENCE_THRESHOLD) {
            Serial.println(">>> VERMELHO DETECTADO! — LED vermelho LIGADO");
            digitalWrite(LED_VERMELHO, HIGH);

        } else {
            Serial.println(">>> Abaixo do threshold — nenhuma acao");
        }

        Serial.println("================================\n");
    }
}

// ============================================================
// SETUP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println("\n======================================");
    Serial.println("     KABUM - DETECTOR DE ANOMALIAS");
    Serial.println("======================================");
    Serial.printf("Sample rate    : %d Hz\n",     SAMPLE_RATE);
    Serial.printf("Window samples : %d\n",         WINDOW_SAMPLES);
    Serial.printf("Window duration: %.2f s\n",     (float)WINDOW_SAMPLES / SAMPLE_RATE);
    Serial.printf("Threshold      : %.2f\n",       CONFIDENCE_THRESHOLD);

    // --- GPIOs ---
    // Inicializa o nível ANTES de habilitar como OUTPUT
    // para evitar pulso espúrio no boot (importante pro relê)
    digitalWrite(LED_VERMELHO, LOW);
    digitalWrite(LED_VERDE,    LOW);
    digitalWrite(RELE,         RELE_OFF);  // HIGH = relê desligado (active-low)

    pinMode(LED_VERMELHO, OUTPUT);
    pinMode(LED_VERDE,    OUTPUT);
    pinMode(RELE,         OUTPUT);

    // --- I2S ---
    if (!init_i2s()) {
        Serial.println("FATAL: falha no I2S. Sistema travado.");
        while (true) delay(1000);
    }

    // --- Semáforo binário (Task1 → Task2) ---
    audioBufferSemaphore = xSemaphoreCreateBinary();
    if (audioBufferSemaphore == NULL) {
        Serial.println("FATAL: falha ao criar semáforo.");
        while (true) delay(1000);
    }

    // --- Fila de features (Task2 → Task3), 2 slots ---
    featureQueue = xQueueCreate(2, sizeof(AudioFeatures));
    if (featureQueue == NULL) {
        Serial.println("FATAL: falha ao criar featureQueue.");
        while (true) delay(1000);
    }

    // --- Tasks ---
    xTaskCreatePinnedToCore(task_audio_capture,      "AudioCapture",     8192, NULL, 5, &taskHandleAudioCapture,      1);
    xTaskCreatePinnedToCore(task_feature_extraction, "FeatureExtraction", 8192, NULL, 3, &taskHandleFeatureExtraction, 0);
    xTaskCreatePinnedToCore(task_anomaly_detection,  "AnomalyDetection",  8192, NULL, 1, &taskHandleAnomalyDetection,  0);

    Serial.println("\n======================================");
    Serial.println("Sistema FreeRTOS iniciado!");
    Serial.println("Task 1: AudioCapture      | P5 | Core 1");
    Serial.println("Task 2: FeatureExtraction | P3 | Core 0");
    Serial.println("Task 3: AnomalyDetection  | P1 | Core 0");
    Serial.println("======================================\n");
}

// ============================================================
// LOOP — todo o trabalho está nas tasks
// ============================================================

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}