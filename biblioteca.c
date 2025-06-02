#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "lib/ssd1306.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "pico/bootrom.h"
#include "stdio.h"

// === CONFIGURAÇÕES ===

// Barramento I2C do display OLED
#define I2C_PORT i2c1
#define I2C_SDA  14
#define I2C_SCL  15
#define ENDERECO 0x3C

// Pinos dos LEDs RGB
#define LED_RED    13
#define LED_GREEN  11
#define LED_BLUE   12

// Buzzer
#define BUZZER_PIN 21

// Botões
#define BOTAO_A    5   // Entrada
#define BOTAO_B    6   // Saída
#define SW_PIN     22  // Reset (joystick)

// Capacidade máxima de vagas
#define MAX_VAGAS 10

// Tempo de debounce (em ticks)
#define DEBOUNCE_TICKS pdMS_TO_TICKS(200)

// === VARIÁVEIS GLOBAIS ===

static volatile TickType_t ultimo_tick_A  = 0;
static volatile TickType_t ultimo_tick_B  = 0;
static volatile TickType_t ultimo_tick_SW = 0;

ssd1306_t ssd;

SemaphoreHandle_t xDisplayMutex;
SemaphoreHandle_t xCounterSemaphore;
SemaphoreHandle_t xResetSemaphore;

// Estado do botão pressionado:
// 0 = nenhum | 1 = entrada | 2 = saída
int evento_botao = 0;

// === FUNÇÕES ===

// Atualiza os LEDs com base na ocupação
void atualizar_led(uint16_t contagem) {
    gpio_put(LED_RED, 0);
    gpio_put(LED_GREEN, 0);
    gpio_put(LED_BLUE, 0);

    if (contagem == 0) {
        gpio_put(LED_BLUE, 1);  // Nenhum usuário
    } else if (contagem <= (MAX_VAGAS - 2)) {
        gpio_put(LED_GREEN, 1); // Espaço suficiente
    } else if (contagem == (MAX_VAGAS - 1)) {
        gpio_put(LED_GREEN, 1);
        gpio_put(LED_RED, 1);   // Quase cheio (amarelo)
    } else if (contagem >= MAX_VAGAS) {
        gpio_put(LED_RED, 1);   // Cheio
    }
}

// Interrupções dos botões
void gpio_irq_handler(uint gpio, uint32_t events) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    TickType_t tick_atual = xTaskGetTickCountFromISR();

    if (gpio == BOTAO_A && (tick_atual - ultimo_tick_A) >= DEBOUNCE_TICKS) {
        ultimo_tick_A = tick_atual;
        evento_botao = 1;
    } else if (gpio == BOTAO_B && (tick_atual - ultimo_tick_B) >= DEBOUNCE_TICKS) {
        ultimo_tick_B = tick_atual;
        evento_botao = 2;
    } else if (gpio == SW_PIN && (tick_atual - ultimo_tick_SW) >= DEBOUNCE_TICKS) {
        ultimo_tick_SW = tick_atual;
        xSemaphoreGiveFromISR(xResetSemaphore, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

// === TAREFAS ===

// Entrada de usuários (BOTAO_A)
void vEntradaTask(void *params) {
    char buffer_ocupadas[20];
    char buffer_vagas[20];

    while (true) {
        if (evento_botao == 1) {
            if (uxSemaphoreGetCount(xCounterSemaphore) > 0) {
                if (xSemaphoreTake(xCounterSemaphore, portMAX_DELAY) == pdTRUE) {
                    UBaseType_t ocupadas = MAX_VAGAS - uxSemaphoreGetCount(xCounterSemaphore);
                    UBaseType_t vagas    = uxSemaphoreGetCount(xCounterSemaphore);

                    ssd1306_fill(&ssd, 0);
                    if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
                        if (ocupadas >= MAX_VAGAS) {
                            ssd1306_draw_string(&ssd, "LOTADO", 35, 30);
                            pwm_set_gpio_level(BUZZER_PIN, 7812);
                            vTaskDelay(pdMS_TO_TICKS(500));
                            pwm_set_gpio_level(BUZZER_PIN, 0);
                        } else {
                            sprintf(buffer_ocupadas, "Ocupado: %d", ocupadas);
                            sprintf(buffer_vagas, "Vagas: %d", vagas);
                            ssd1306_draw_string(&ssd, buffer_vagas, 5, 20);
                            ssd1306_draw_string(&ssd, buffer_ocupadas, 5, 44);
                        }
                        ssd1306_send_data(&ssd);
                        xSemaphoreGive(xDisplayMutex);
                    }
                    atualizar_led(ocupadas);
                    evento_botao = 0;
                }
            }
        }
        vTaskDelay(1);
    }
}

// Saída de usuários (BOTAO_B)
void vSaidaTask(void *params) {
    char buffer_ocupadas[20];
    char buffer_vagas[20];

    while (true) {
        if (evento_botao == 2) {
            xSemaphoreGive(xCounterSemaphore);

            UBaseType_t ocupadas = MAX_VAGAS - uxSemaphoreGetCount(xCounterSemaphore);
            UBaseType_t vagas    = uxSemaphoreGetCount(xCounterSemaphore);

            if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
                ssd1306_fill(&ssd, 0);
                sprintf(buffer_ocupadas, "Ocupado: %d", ocupadas);
                sprintf(buffer_vagas, "Vagas: %d", vagas);
                ssd1306_draw_string(&ssd, buffer_vagas, 5, 20);
                ssd1306_draw_string(&ssd, buffer_ocupadas, 5, 44);
                ssd1306_send_data(&ssd);
                xSemaphoreGive(xDisplayMutex);
            }

            atualizar_led(ocupadas);
            evento_botao = 0;
        }
        vTaskDelay(1);
    }
}

// Reset do sistema (SW_PIN)
void vTaskReset(void *params) {
    while (true) {
        if (xSemaphoreTake(xResetSemaphore, portMAX_DELAY) == pdTRUE) {
            while (uxSemaphoreGetCount(xCounterSemaphore) < MAX_VAGAS) {
                xSemaphoreGive(xCounterSemaphore);
            }

            if (xSemaphoreTake(xDisplayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                ssd1306_fill(&ssd, 0);
                ssd1306_draw_string(&ssd, "Resetado!", 5, 19);
                ssd1306_draw_string(&ssd, "Ocupado: 0", 5, 44);
                ssd1306_send_data(&ssd);

                // Beep duplo
                for (int i = 0; i < 2; i++) {
                    pwm_set_gpio_level(BUZZER_PIN, 7812);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    pwm_set_gpio_level(BUZZER_PIN, 0);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }

                atualizar_led(0);
                xSemaphoreGive(xDisplayMutex);
            }
        }
    }
}

// === FUNÇÃO PRINCIPAL ===

int main() {
    stdio_init_all();

    // Inicialização I2C + display
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    ssd1306_init(&ssd, WIDTH, HEIGHT, false, ENDERECO, I2C_PORT);
    ssd1306_config(&ssd);
    ssd1306_send_data(&ssd);

    // Configuração dos botões
    gpio_init(BOTAO_A); gpio_set_dir(BOTAO_A, GPIO_IN); gpio_pull_up(BOTAO_A);
    gpio_init(BOTAO_B); gpio_set_dir(BOTAO_B, GPIO_IN); gpio_pull_up(BOTAO_B);
    gpio_init(SW_PIN);  gpio_set_dir(SW_PIN,  GPIO_IN); gpio_pull_up(SW_PIN);

    // Configuração de LEDs e buzzer
    gpio_init(LED_RED);    gpio_set_dir(LED_RED, GPIO_OUT);
    gpio_init(LED_GREEN);  gpio_set_dir(LED_GREEN, GPIO_OUT);
    gpio_init(LED_BLUE);   gpio_set_dir(LED_BLUE, GPIO_OUT);
    gpio_init(BUZZER_PIN); gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);

    // PWM para o buzzer
    uint slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_wrap(slice, 12500);
    pwm_set_chan_level(slice, PWM_CHAN_A, 0);
    pwm_set_enabled(slice, true);

    // Inicializa semáforos
    xCounterSemaphore  = xSemaphoreCreateCounting(MAX_VAGAS, MAX_VAGAS);
    xDisplayMutex      = xSemaphoreCreateMutex();
    xResetSemaphore    = xSemaphoreCreateBinary();

    // Configura interrupções dos botões
    gpio_set_irq_enabled_with_callback(BOTAO_A, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);
    gpio_set_irq_enabled(BOTAO_B, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(SW_PIN, GPIO_IRQ_EDGE_FALL, true);

    // Cria tarefas
    xTaskCreate(vEntradaTask, "Entrada", 256, NULL, 1, NULL);
    xTaskCreate(vSaidaTask,   "Saida",   256, NULL, 1, NULL);
    xTaskCreate(vTaskReset,   "Reset",   256, NULL, 1, NULL);

    // Inicia o escalonador
    vTaskStartScheduler();

   // Caso o scheduler não inicie, entra em modo de erro
    panic_unsupported();
}
