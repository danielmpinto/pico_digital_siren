#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/timer.h"

// =========================================================================
// DEFINIÇÕES E CONSTANTES
// =========================================================================
#define MAX_VOZES 4
#define TAXA_AMOSTRAGEM 45454 // ~45.4 kHz (-22 microsegundos no timer)

// Pinos
#define PINO_AUDIO 26         // GP26 -> PWM Slice 5 Canal A (Saída para filtro RC -> P10)
#define PINO_LED_VERDE 2      // GP2  -> PWM Slice 1 Canal A (Brilho proporcional)
#define PINO_LED_AZUL 3       // GP3  -> GPIO Digital (Blink de áudio ativo)
#define PINO_BTN_VERDE 13     // GP13 -> Polling (Segurar para ajustar duração até 5s)
#define PINO_BTN_AZUL 14      // GP14 -> IRQ (Disparo da sirene com sobreposição)
#define PINO_BTN_VERMELHO 15  // GP15 -> IRQ (Kill Switch)

// =========================================================================
// ESTRUTURAS E ESTADO
// =========================================================================
typedef struct {
    bool ativa;
    int amostras_restantes;
    float frequencia_atual;
    float fase_vco;
    float fase_lfo;
} VozSirene;

VozSirene vozes[MAX_VOZES];
uint slice_audio;
uint slice_led_verde;

volatile int duracao_ms = 1000; // Começa em 1 segundo
volatile int vozes_ativas_globais = 0;

volatile uint32_t ultimo_aperto_azul = 0;
volatile uint32_t ultimo_aperto_vermelho = 0;
volatile bool kill_switch_acionado = false;

// =========================================================================
// SÍNTESE DE ÁUDIO (Interrupção de Timer)
// =========================================================================
void disparar_sirene() {
    for (int i = 0; i < MAX_VOZES; i++) {
        if (!vozes[i].ativa) {
            vozes[i].ativa = true;
            vozes[i].amostras_restantes = (duracao_ms * TAXA_AMOSTRAGEM) / 1000;
            vozes[i].frequencia_atual = 2000.0f; // Frequência inicial alta
            vozes[i].fase_vco = 0.0f;
            vozes[i].fase_lfo = 0.0f;
            break;
        }
    }
}

bool audio_timer_callback(struct repeating_timer *t) {
    int soma_audio = 0;
    int ativas = 0;

    for (int i = 0; i < MAX_VOZES; i++) {
        if (vozes[i].ativa) {
            ativas++;

            // 1. Decaimento do pitch
            vozes[i].frequencia_atual *= 0.9995f;

            // 2. LFO rápido (bolhas a ~20Hz)
            vozes[i].fase_lfo += (20.0f / TAXA_AMOSTRAGEM);
            if (vozes[i].fase_lfo >= 1.0f) vozes[i].fase_lfo -= 1.0f;
            float lfo = (vozes[i].fase_lfo < 0.5f) ? 1.0f : -1.0f;

            // 3. Frequência modulada
            float freq_vco = vozes[i].frequencia_atual + (lfo * 300.0f);
            if (freq_vco < 20.0f) freq_vco = 20.0f;

            // 4. Oscilador da onda sonora
            vozes[i].fase_vco += (freq_vco / TAXA_AMOSTRAGEM);
            if (vozes[i].fase_vco >= 1.0f) vozes[i].fase_vco -= 1.0f;

            int amostra = (vozes[i].fase_vco < 0.5f) ? 255 : 0;
            soma_audio += amostra;

            // 5. Contagem de amostras
            vozes[i].amostras_restantes--;
            if (vozes[i].amostras_restantes <= 0) {
                vozes[i].ativa = false;
            }
        }
    }

    vozes_ativas_globais = ativas;

    // Envio para o hardware PWM
    if (ativas > 0) {
        pwm_set_gpio_level(PINO_AUDIO, (uint16_t)(soma_audio / MAX_VOZES));
    } else {
        pwm_set_gpio_level(PINO_AUDIO, 0);
    }

    return true;
}

// =========================================================================
// INTERRUPÇÃO DOS BOTÕES (IRQ)
// =========================================================================
void botoes_irq_callback(uint gpio, uint32_t eventos) {
    uint32_t tempo_agora = to_ms_since_boot(get_absolute_time());

    if (gpio == PINO_BTN_AZUL) {
        if (tempo_agora - ultimo_aperto_azul > 150) { // Debounce de 150ms
            disparar_sirene();
            ultimo_aperto_azul = tempo_agora;
        }
    } 
    else if (gpio == PINO_BTN_VERMELHO) {
        if (tempo_agora - ultimo_aperto_vermelho > 150) {
            for (int i = 0; i < MAX_VOZES; i++) {
                vozes[i].ativa = false;
            }
            kill_switch_acionado = true;
            ultimo_aperto_vermelho = tempo_agora;
        }
    }
}

// =========================================================================
// INICIALIZAÇÃO DE HARDWARE
// =========================================================================
void setup_hardware() {
    stdio_init_all();

    // 1. Áudio PWM (GP26)
    gpio_set_function(PINO_AUDIO, GPIO_FUNC_PWM);
    slice_audio = pwm_gpio_to_slice_num(PINO_AUDIO);
    pwm_set_wrap(slice_audio, 255);
    pwm_set_chan_level(slice_audio, pwm_gpio_to_channel(PINO_AUDIO), 0);
    pwm_set_enabled(slice_audio, true);

    // 2. LED Verde PWM (GP2)
    gpio_set_function(PINO_LED_VERDE, GPIO_FUNC_PWM);
    slice_led_verde = pwm_gpio_to_slice_num(PINO_LED_VERDE);
    pwm_set_wrap(slice_led_verde, 255);
    pwm_set_chan_level(slice_led_verde, pwm_gpio_to_channel(PINO_LED_VERDE), (1000 * 255) / 5000);
    pwm_set_enabled(slice_led_verde, true);

    // 3. LED Azul Digital (GP3)
    gpio_init(PINO_LED_AZUL);
    gpio_set_dir(PINO_LED_AZUL, GPIO_OUT);
    gpio_put(PINO_LED_AZUL, 0);

    // 4. Botões com Pull-Up interno ativado
    gpio_init(PINO_BTN_VERDE);
    gpio_set_dir(PINO_BTN_VERDE, GPIO_IN);
    gpio_pull_up(PINO_BTN_VERDE);

    gpio_init(PINO_BTN_AZUL);
    gpio_set_dir(PINO_BTN_AZUL, GPIO_IN);
    gpio_pull_up(PINO_BTN_AZUL);

    gpio_init(PINO_BTN_VERMELHO);
    gpio_set_dir(PINO_BTN_VERMELHO, GPIO_IN);
    gpio_pull_up(PINO_BTN_VERMELHO);

    // 5. Configuração das IRQs dos botões
    gpio_set_irq_enabled_with_callback(PINO_BTN_AZUL, GPIO_IRQ_EDGE_FALL, true, &botoes_irq_callback);
    gpio_set_irq_enabled(PINO_BTN_VERMELHO, GPIO_IRQ_EDGE_FALL, true);

    for (int i = 0; i < MAX_VOZES; i++) {
        vozes[i].ativa = false;
    }
}

// =========================================================================
// LOOP PRINCIPAL
// =========================================================================
int main() {
    setup_hardware();

    // Timer de áudio em -22 us (~45.4 kHz)
    struct repeating_timer timer;
    add_repeating_timer_us(-22, audio_timer_callback, NULL, &timer);

    uint32_t ultimo_blink = 0;
    bool estado_led_azul = false;

    while (1) {
        // Trata Kill Switch
        if (kill_switch_acionado) {
            duracao_ms = 1000;
            pwm_set_gpio_level(PINO_LED_VERDE, (1000 * 255) / 5000);
            kill_switch_acionado = false;
        }

        // Incremento contínuo ao segurar botão verde
        if (!gpio_get(PINO_BTN_VERDE)) {
            duracao_ms += 15;
            if (duracao_ms > 5000) duracao_ms = 5000;

            int brilho = (duracao_ms * 255) / 5000;
            pwm_set_gpio_level(PINO_LED_VERDE, brilho);
            sleep_ms(15);
        }

        // Piscar LED azul durante execução do áudio
        if (vozes_ativas_globais > 0) {
            uint32_t agora = to_ms_since_boot(get_absolute_time());
            if (agora - ultimo_blink > 80) {
                estado_led_azul = !estado_led_azul;
                gpio_put(PINO_LED_AZUL, estado_led_azul);
                ultimo_blink = agora;
            }
        } else {
            estado_led_azul = false;
            gpio_put(PINO_LED_AZUL, 0);
        }
    }

    return 0;
}