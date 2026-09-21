# Relatório Técnico — Detector de Anomalias Acústicas com TinyML no ESP32

**Disciplina:** Computação Embarcada — TinyML / Edge Computing  
**Data de Entrega:** 18 de setembro de 2026  
**Demonstração:** 21 de setembro de 2026

---

## 1. Introdução e Justificativa da Aplicação

Este relatório descreve o projeto de um sistema embarcado de detecção de anomalias acústicas em tempo real, implementado em um microcontrolador ESP32 equipado com microfone digital INMP441. O objetivo central é demonstrar como conceitos de RTOS (Real-Time Operating System), processamento de sinais em edge computing e inferência de redes neurais embarcadas podem ser integrados em um único dispositivo de baixo custo e recursos limitados.

A aplicação escolhida é o reconhecimento de três comandos de voz específicos: **"Kabum"**, **"Azul"** e **"Vermelho"**. Cada classe reconhecida aciona um atuador físico diferente — relé ou LEDs coloridos — permitindo transformar padrões acústicos em respostas físicas imediatas. A classe `unknown` representa silêncio ou ruído não classificado.

A justificativa prática desta escolha reside na demonstrabilidade em bancada: palavras curtas (monossilábicas a trissilábicas) são padrões acústicos bem definidos, com energia espectral concentrada e separabilidade clara em espaço de características MFCCs. O mesmo pipeline pode ser adaptado a qualquer anomalia sonora — quedas, alarmes, latidos — com apenas a substituição do modelo treinado.

---

## 2. Arquitetura RTOS

### 2.1 Visão Geral do Sistema

O firmware de classificação segue uma arquitetura de pipeline orientada a tarefas, dividindo o processamento em três estágios concorrentes gerenciados pelo FreeRTOS. Os dois núcleos do processador Xtensa dual-core LX6 são utilizados de forma complementar: o **Core 1** é dedicado exclusivamente à captura de áudio (Task 1), enquanto o **Core 0** executa o processamento e a inferência (Tasks 2 e 3).

```
┌─────────────────────────────────────────────────────────────────────┐
│                         CORE 1 (Prioridade Alta)                    │
│                                                                     │
│  [INMP441 I2S RX] ──► Task 1: AudioCapture (P5)                    │
│                        16 kHz • Double Buffering                    │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ audioBufferSemaphore
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         CORE 0 (Processamento e IA)                 │
│                                                                     │
│  Task 2: FeatureExtraction (P3) ──featureQueue──► Task 3: AnomalyDetection (P1)
│  RMS + Spectral Centroid                          Edge Impulse DSP + CNN
└──────────────────────────────┬──────────────────────────────────────┘
                               │ digitalWrite()
                    ┌──────────┴──────────┐
                    ▼          ▼          ▼
              [Relé GPIO22] [LED Azul] [LED Vermelho]
                 (Kabum)    (GPIO 19)   (GPIO 18)
```

### 2.2 Detalhamento das Tarefas

| Tarefa | Prioridade | Núcleo | Stack | Responsabilidade |
|---|---|---|---|---|
| `task_audio_capture` | **5 (Alta)** | Core 1 | 8192 B | Captura contínua via I2S. Double buffering. Sinaliza semáforo ao completar janela de 1 s. |
| `task_feature_extraction` | **3 (Média)** | Core 0 | 8192 B | Aguarda semáforo. Calcula RMS e Spectral Centroid. Enfileira `AudioFeatures`. |
| `task_anomaly_detection` | **1 (Baixa)** | Core 0 | 8192 B | Consome fila. Normaliza sinal. Executa DSP + inferência via Edge Impulse. Aciona atuadores. |

### 2.3 Mecanismos de Sincronização

**Semáforo Binário (`audioBufferSemaphore`):** produzido pela Task 1 ao terminar de preencher um buffer e consumido pela Task 2. Garante que a extração de features só ocorra sobre dados completos, sem polling ativo — a Task 2 fica bloqueada em `xSemaphoreTake(portMAX_DELAY)` até haver dados disponíveis.

**Fila (`featureQueue`, 2 slots):** canal assíncrono entre Task 2 e Task 3. Transfere a estrutura `AudioFeatures` (RMS, Spectral Centroid, ponteiro para o buffer de áudio e timestamp de latência). Dois slots permitem que a Task 2 continue processando sem aguardar a inferência. Se a fila encher, a Task 2 bloqueia naturalmente — backpressure automático.

**Seção Crítica (`portMUX_TYPE / bufferMux`):** proteção atômica para leitura e escrita das flags de controle de double buffering (`buffer_a_ready / buffer_b_ready`). Evita race conditions entre os dois núcleos ao trocar os ponteiros de buffer.

**Double Buffering (`audio_buffer_a / audio_buffer_b`):** enquanto a Task 1 preenche um buffer, a Task 2/3 processa o outro. Elimina a necessidade de pausar a captura durante o processamento.

### 2.4 Fluxo de Dados entre Tarefas

| Etapa | De | Para | Mecanismo | Dado Transferido |
|---|---|---|---|---|
| 1 → 2 | Task 1 | Task 2 | Semáforo + `portMUX` | Ponteiro para buffer de áudio (16.000 amostras `int16`) |
| 2 → 3 | Task 2 | Task 3 | Fila FreeRTOS (2 slots) | `struct AudioFeatures {rms, centroid, tempo, ponteiro}` |
| 3 → HW | Task 3 | GPIO / Atuadores | `digitalWrite()` | Sinal digital para relé (GPIO 22) ou LEDs (GPIO 18/19) |

### 2.5 Configuração do Hardware I2S

| Pino INMP441 | GPIO ESP32 | Função |
|---|---|---|
| SCK / BCLK | GPIO 26 | Bit Clock — clock serial contínuo |
| WS / LRC | GPIO 25 | Word Select — identificação de canal (esquerdo) |
| SD / DATA | GPIO 33 | Serial Data — linha de áudio |
| VDD | 3.3 V | Alimentação (nunca usar 5 V) |
| L/R | GND | Força canal mono esquerdo |

O driver é configurado em modo RX a 16 kHz, 32 bits por amostra (24 bits úteis do microfone), com 8 buffers DMA de 512 amostras cada. As amostras recebidas em `int32` são deslocadas 14 bits para a direita (`>> 14`) para obter valores `int16` compatíveis com o modelo Edge Impulse.

---

## 3. Modelo de Detecção de Anomalias

### 3.1 Pipeline de Treinamento

**Coleta de dados:** o firmware `recorder.ino` captura áudio a 16 kHz e transmite arquivos WAV de 60 segundos via serial. O script Python `dataset_collector.py` recebe e salva os arquivos, que são segmentados em janelas de 1 segundo no Audacity e importados no Edge Impulse Studio.

**Extração de features:** o bloco DSP do Edge Impulse aplica janelamento Hamming, FFT de 256 pontos e filtragem em banco de 40 filtros Mel para obter 13 coeficientes MFCCs por frame. Com `frame_length=0.02 s` e `frame_stride=0.01 s`, cada janela de 1 segundo gera uma matriz de features **99 × 13 = 1287 valores**.

**Arquitetura da rede neural:** rede convolucional (CNN) 1D com camadas Conv1D → BatchNorm → ReLU → MaxPooling, seguida de camadas densas e softmax de 4 classes. O modelo foi quantizado para INT8 (TFLite) para execução no ESP32.

### 3.2 Formatos de Exportação

| Arquivo | Formato | Uso |
|---|---|---|
| `kabum_model.tflite` | TensorFlow Lite INT8 | Inferência embarcada no ESP32 via Edge Impulse SDK |
| `kabum_model.onnx` | Open Neural Network Exchange | Validação em Python, auditoria e entregável acadêmico |
| `lib.zip` | Biblioteca C++ Arduino | Pacote completo com DSP + kernels + inferência pronta |

### 3.3 Classes e Regra de Decisão

Threshold de confiança: **`CONFIDENCE_THRESHOLD = 0.70`** (70%).

| Classe | Score ≥ 0.70? | Atuador | GPIO |
|---|---|---|---|
| `Kabum` | Sim | Relé LIGADO (active-LOW) | GPIO 22 |
| `Azul` | Sim | LED Azul ACESO | GPIO 19 |
| `Vermelho` | Sim | LED Vermelho ACESO | GPIO 18 |
| `unknown` / < 0.70 | Não | Todos desligados | — |

Antes de qualquer acionamento, todos os atuadores são explicitamente desligados, garantindo que apenas um atuador fique ativo por janela de inferência e eliminando sobreposição de estados entre frames consecutivos.

---

## 4. Análise de Latência

### 4.1 Metodologia de Medição

A latência de cada etapa é medida com a função `micros()` do Arduino, que retorna o tempo em microssegundos desde o boot. Os timestamps são inseridos imediatamente antes e após cada operação crítica e reportados via Serial a 115200 baud para análise offline.

Etapas medidas:
- **Task 1 — Captura de Áudio:** tempo desde o primeiro `i2s_read()` até o preenchimento completo da janela de 16.000 amostras.
- **Task 2 — Extração de Features:** tempo desde o recebimento do ponteiro de buffer até o envio da `struct AudioFeatures` para a fila. Mede o custo de `calculate_rms()` e `calculate_spectral_centroid()`.
- **Task 3 — Inferência (DSP + rede neural):** tempo desde a chamada `run_classifier()` até o retorno com `ei_impulse_result_t`. Inclui o pré-processamento MFCC e a passagem pela rede neural.

### 4.2 Resultados Medidos

| Etapa | Tempo Típico | Tempo Máximo | Observações |
|---|---|---|---|
| Task 1 — Captura (1 s de áudio) | ~1.000.000 µs (1,0 s) | 1.010.000 µs | Determinístico: limitado pela taxa 16 kHz × 16.000 amostras |
| Task 2 — RMS + Centroid | ~1.845 µs | ~2.500 µs | Dominado por acesso a memória; custo negligenciável no pipeline |
| Task 3 — DSP (MFCC) | ~15.000 µs | ~18.000 µs | Janelamento + FFT + filtro Mel — operação mais custosa |
| Task 3 — Inferência (CNN) | ~3.000 µs | ~4.000 µs | Rede quantizada INT8; muito eficiente no Xtensa LX6 |
| Task 3 — Total (DSP + CNN) | ~18.230 µs | ~22.000 µs | Soma das duas sub-etapas acima |
| **Latência Total do Pipeline** | **~1.020.000 µs (~1,02 s)** | **~1.032.000 µs** | Dominada pelo tempo de captura — cadência de inferência ~1 Hz |

### 4.3 Análise e Discussão de Latência

A latência total do pipeline é dominada pelo tempo de captura de áudio (~1 s), o que é esperado e adequado para o modelo treinado com janelas de 1 segundo. O processamento (Tasks 2 e 3) acrescenta apenas **~20 ms adicionais (~2%)**, demonstrando que o sistema é eficiente e não introduz atrasos significativos além do necessário.

O design de double buffering é fundamental: enquanto a Task 3 executa a inferência sobre o buffer processado, a Task 1 já está preenchendo o próximo buffer. Na prática, a latência ponta-a-ponta (evento sonoro → atuação física) é de aproximadamente **1 s a 2 s**, dependendo do instante em que o evento ocorre dentro da janela de captura.

O FreeRTOS scheduler garante que a Task 1 (prioridade 5) não seja preemptada pelas Tasks 2 e 3 (prioridades 3 e 1). Medições de jitter na captura mostram variação inferior a 1% do período, confirmando comportamento determinístico adequado para RTOS.

> Para aplicações que exigem detecção mais rápida (< 200 ms), seria necessário reduzir a janela de inferência para 0,5 s ou menos, o que exigiria re-treinar o modelo com janelas menores.

---

## 5. Resultados

### 5.1 Acurácia do Modelo

| Classe | Precisão | Recall | F1-Score | Amostras de Validação |
|---|---|---|---|---|
| Kabum | 96,2% | 95,8% | 96,0% | ~50 amostras |
| Azul | 94,5% | 93,1% | 93,8% | ~50 amostras |
| Vermelho | 95,0% | 94,6% | 94,8% | ~50 amostras |
| unknown | 97,3% | 98,1% | 97,7% | ~80 amostras |
| **Acurácia Global** | **95,8%** | — | — | **~230 amostras** |

### 5.2 Testes de Detecção em Tempo Real

| Cenário | Comportamento Esperado | Resultado Observado |
|---|---|---|
| "Kabum" pronunciado claramente | Relé liga (GPIO 22 LOW) | Relé ativado; score típico > 0,90 |
| "Azul" pronunciado | LED azul acende (GPIO 19) | LED aceso; score típico > 0,85 |
| "Vermelho" pronunciado | LED vermelho acende (GPIO 18) | LED aceso; score típico > 0,88 |
| Silêncio / ruído ambiente | Todos atuadores desligados | Classe `unknown` dominante; nenhum acionamento |
| Palavras aleatórias | Todos atuadores desligados | Score das classes-alvo < 0,70 na maioria dos casos |
| Comando com ruído moderado | Detecção com score reduzido | Score 0,75–0,85; ainda detecta corretamente |

### 5.3 Exemplo de Saída Serial

Log real do monitor serial durante inferência com "Kabum":

```
======================================
     KABUM - DETECTOR DE ANOMALIAS
======================================
Sample rate    : 16000 Hz
Window samples : 16000
Window duration: 1.00 s
Threshold      : 0.70
Inicializando I2S...
I2S OK

======================================
Sistema FreeRTOS iniciado!
Task 1: AudioCapture      | P5 | Core 1
Task 2: FeatureExtraction | P3 | Core 0
Task 3: AnomalyDetection  | P1 | Core 0
======================================

[TASK 1] 16000 samples em 1000120 us
[TASK 2] RMS: 0.0821 | Centroid: 1420.5 Hz | 1845 us

========== RESULTADO ==========
  Azul       : 0.02150
  Kabum      : 0.94210
  Vermelho   : 0.01120
  unknown    : 0.02520
  Inferencia    : 18230 us
  Feature extr. : 1845 us
>>> KABUM DETECTADO! — Relê LIGADO
================================
```

---

## 6. Implementação de Concorrência e Resolução de Conflitos

### Conflito 1 — Acesso concorrente ao buffer de áudio

**Problema:** a Task 1 escreve no buffer enquanto a Task 2 pode tentar lê-lo.

**Solução:** double buffering com troca atômica de ponteiros sob seção crítica (`portMUX_TYPE`). As flags `buffer_a_ready` / `buffer_b_ready` são lidas e escritas apenas dentro de `portENTER_CRITICAL` / `portEXIT_CRITICAL`, eliminando race conditions entre os dois núcleos.

### Conflito 2 — Sincronização produtor/consumidor (Task 1 → Task 2)

**Problema:** a Task 2 não pode processar antes de a Task 1 ter preenchido um buffer completo.

**Solução:** semáforo binário (`xSemaphoreGive` / `xSemaphoreTake`). A Task 2 fica bloqueada em `xSemaphoreTake(portMAX_DELAY)` sem consumir CPU, e é acordada apenas quando a Task 1 sinaliza a conclusão do buffer.

### Conflito 3 — Desacoplamento entre extração e inferência (Task 2 → Task 3)

**Problema:** a inferência é mais lenta que a extração de features, podendo gerar bloqueio.

**Solução:** fila FreeRTOS com 2 slots (`xQueueCreate(2, sizeof(AudioFeatures))`). A Task 2 pode continuar processando o próximo buffer sem aguardar a Task 3 finalizar. Se a fila encher, a Task 2 bloqueia em `xQueueSend(portMAX_DELAY)`, gerando backpressure natural e sem perda de dados.

### Conflito 4 — Estado dos atuadores entre janelas consecutivas

**Problema:** uma classe detectada em um frame pode conflitar com a detecção do próximo frame.

**Solução:** reset explícito de todos os atuadores (LED e relé) no início de cada ciclo de decisão na Task 3, antes de qualquer acionamento. Garante estado limpo a cada inferência.

---

## 7. Discussão

### 7.1 Adequação da Arquitetura ao Problema

A separação em três tarefas com prioridades distintas reflete diretamente os requisitos de tempo real do problema. A captura de áudio (Task 1) tem prioridade máxima pois qualquer interrupção causaria perda de amostras, corrompendo a janela de 1 segundo. A inferência (Task 3) tem prioridade mínima pois pode tolerar pequenos atrasos sem impacto na qualidade da detecção.

A distribuição em dois núcleos físicos isola completamente a captura de áudio (Core 1) do processamento pesado (Core 0), eliminando a necessidade de preempção entre Task 1 e Tasks 2/3. Isso reduz jitter de captura e garante a taxa de amostragem constante de 16 kHz.

### 7.2 Limitações e Trabalhos Futuros

**Latência de 1 s:** a cadência de inferência de ~1 Hz é adequada para comandos de voz, mas insuficiente para eventos transitórios (quedas, cliques). Trabalho futuro: janelas sobrepostas com overlap de 50%, reduzindo a latência efetiva para ~500 ms.

**Ruído de ambiente:** em ambientes com ruído variável, a acurácia pode degradar. Trabalho futuro: adicionar pré-processamento de supressão de ruído (VAD — Voice Activity Detection) ou expandir o dataset com data augmentation de amostras ruidosas.

**Vocabulário fixo:** o modelo atual reconhece apenas três palavras-chave. Trabalho futuro: treinar com vocabulário expandido ou integrar modelo de linguagem menor para compreensão semântica.

**Consumo de energia:** o ESP32 em operação contínua consome ~80–100 mA. Trabalho futuro: explorar deep sleep entre janelas de inferência com wake-up por trigger de energia (RMS threshold) para aplicações com bateria.

### 7.3 Comparativo com Abordagens Alternativas

| Abordagem | Vantagem | Desvantagem vs. Este Projeto |
|---|---|---|
| Processamento na nuvem (streaming) | Modelo maior, maior acurácia | Latência > 200 ms RTT, requer Wi-Fi, sem privacidade |
| Edge sem RTOS (loop único) | Implementação simples | Perde amostras durante inferência, sem garantias temporais |
| ARM Cortex-M7 (STM32H7) | Maior poder de processamento | Maior custo, consumo e complexidade de toolchain |
| ESP32-S3 com acelerador de IA | Inferência ~5× mais rápida | Componente diferente; mesmo princípio arquitetural |

---

## 8. Conclusão

Este projeto demonstrou com sucesso a viabilidade de um sistema embarcado de detecção de anomalias acústicas em tempo real no ESP32, integrando os conceitos de RTOS multitarefa, processamento de sinais em edge computing e inferência de TinyML.

A arquitetura de três tarefas concorrentes sincronizadas por semáforo e fila FreeRTOS garante captura contínua sem perda de amostras, extração de features com medição de latência e inferência com resposta física imediata — tudo em um dispositivo de poucos dólares.

Os resultados obtidos — acurácia global de ~95,8%, latência de features de ~1,8 ms, latência de inferência de ~18 ms e taxa de detecção consistente acima de 70% de confiança — confirmam que a abordagem TinyML com Edge Impulse no ESP32 é matura o suficiente para aplicações práticas de IoT embarcado.

O pipeline completo — coleta com `dataset_collector.py`, treinamento no Edge Impulse Studio, exportação para TFLite/ONNX e deploy com FreeRTOS multicore — representa um ciclo de desenvolvimento realista e reproduzível para qualquer categoria de anomalia acústica.

---

## 9. Referências

- Espressif Systems. *ESP32 Technical Reference Manual v5.4*. 2024.
- Edge Impulse, Inc. *Edge Impulse Documentation — Audio Classification*. 2024. Disponível em: docs.edgeimpulse.com
- Warden, P.; Situnayake, D. *TinyML: Machine Learning with TensorFlow Lite on Arduino and Ultra-Low-Power Microcontrollers*. O'Reilly Media, 2019.
- FreeRTOS. *FreeRTOS Reference Manual — Task Management and Synchronization*. Real Time Engineers Ltd., 2024.
- Goodfellow, I.; Bengio, Y.; Courville, A. *Deep Learning*. MIT Press, 2016.
- Davis, S.; Mermelstein, P. Comparison of parametric representations for monosyllabic word recognition in continuously spoken sentences. *IEEE TASLP*, 1980.