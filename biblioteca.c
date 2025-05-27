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

// Configurações do barramento I2C para o display OLED
#define I2C_PORT i2c1
#define I2C_SDA 14
#define I2C_SCL 15
#define ENDERECO 0x3C

// Pinos dos LEDs RGB
#define LED_RED 13
#define LED_GREEN 11
#define LED_BLUE 12

// Pino do buzzer
#define BUZZER_PIN 21

// Pinos dos botões
#define BOTAO_A 5   // Botão de entrada
#define BOTAO_B 6   // Botão de saída
#define SW_PIN  22  // Botão de reset (joystick)

// Limite máximo de vagas no sistema
#define MAX_VAGAS 10

// Tempo de debounce em ticks (200 ms)
#define DEBOUNCE_TICKS pdMS_TO_TICKS(200)

// Controle de debounce por botão
static volatile TickType_t ultimo_tick_A = 0;
static volatile TickType_t ultimo_tick_B = 0;
static volatile TickType_t ultimo_tick_SW = 0;

// Objeto do display OLED
ssd1306_t ssd;

// Semáforos e mutex
SemaphoreHandle_t xDisplayMutex;
SemaphoreHandle_t xContadorEntrada;
SemaphoreHandle_t xContadorSaida;
SemaphoreHandle_t xResetSemaphore;

// Contador de usuários ativos no sistema
uint16_t usuarios_ativos = 0;

// Atualiza LEDs com base na contagem de usuários
void atualizar_led(uint16_t contagem) {
    gpio_put(LED_RED, 0);
    gpio_put(LED_GREEN, 0);
    gpio_put(LED_BLUE, 0);

    if (contagem == 0) {
        gpio_put(LED_BLUE, 1);  // Nenhum usuário: LED azul
    } else if (contagem <= (MAX_VAGAS - 2)) {
        gpio_put(LED_GREEN, 1);  // Espaço disponível: LED verde
    } else if (contagem == (MAX_VAGAS - 1)) {
        gpio_put(LED_GREEN, 1);
        gpio_put(LED_RED, 1);   // Quase lotado: LED amarelo (verde + vermelho)
    } else if (contagem >= MAX_VAGAS) {
        gpio_put(LED_RED, 1);   // Lotado: LED vermelho
    }
}

// Handler de interrupção para os botões
void gpio_irq_handler(uint gpio, uint32_t events) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    TickType_t tick_atual = xTaskGetTickCountFromISR();

    if (gpio == BOTAO_A && (tick_atual - ultimo_tick_A) >= DEBOUNCE_TICKS) {
        ultimo_tick_A = tick_atual;
        xSemaphoreGiveFromISR(xContadorEntrada, &xHigherPriorityTaskWoken);
    } else if (gpio == BOTAO_B && (tick_atual - ultimo_tick_B) >= DEBOUNCE_TICKS) {
        ultimo_tick_B = tick_atual;
        xSemaphoreGiveFromISR(xContadorSaida, &xHigherPriorityTaskWoken);
    } else if (gpio == SW_PIN && (tick_atual - ultimo_tick_SW) >= DEBOUNCE_TICKS) {
        ultimo_tick_SW = tick_atual;
        xSemaphoreGiveFromISR(xResetSemaphore, &xHigherPriorityTaskWoken);
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// Tarefa responsável por processar entradas (BOTAO_A)
void vEntradaTask(void *params) {
    char buffer_ocupadas[20];
    char buffer_vagas[20];

    while (true) {
        if (xSemaphoreTake(xContadorEntrada, portMAX_DELAY) == pdTRUE && usuarios_ativos < MAX_VAGAS) {
            usuarios_ativos++;
            ssd1306_fill(&ssd, 0);

            if (usuarios_ativos >= MAX_VAGAS) {
                ssd1306_draw_string(&ssd, "LOTADO", 35, 30);
                pwm_set_gpio_level(BUZZER_PIN, 7812);
                vTaskDelay(pdMS_TO_TICKS(500));
                pwm_set_gpio_level(BUZZER_PIN, 0);
            } else {
                sprintf(buffer_ocupadas, "Ocupado: %d", usuarios_ativos);
                sprintf(buffer_vagas, "Vagas: %d", MAX_VAGAS - usuarios_ativos);
                ssd1306_draw_string(&ssd, buffer_vagas, 5, 20);
                ssd1306_draw_string(&ssd, buffer_ocupadas, 5, 44);
            }

            ssd1306_send_data(&ssd);
            xSemaphoreGive(xDisplayMutex);
        }

        atualizar_led(usuarios_ativos);
    }
}

// Tarefa responsável por processar saídas (BOTAO_B)
void vSaidaTask(void *params) {
    char buffer_ocupadas[20];
    char buffer_vagas[20];

    while (true) {
        if (xSemaphoreTake(xContadorSaida, portMAX_DELAY) == pdTRUE && usuarios_ativos > 0) {
            usuarios_ativos--;

            if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
                ssd1306_fill(&ssd, 0);
                sprintf(buffer_ocupadas, "Ocupado: %d", usuarios_ativos);
                sprintf(buffer_vagas, "Vagas: %d", MAX_VAGAS - usuarios_ativos);
                ssd1306_draw_string(&ssd, buffer_vagas, 5, 20);
                ssd1306_draw_string(&ssd, buffer_ocupadas, 5, 44);
                ssd1306_send_data(&ssd);
                xSemaphoreGive(xDisplayMutex);
            }
        }

        atualizar_led(usuarios_ativos);
    }
}

// Tarefa responsável por resetar o sistema (SW_PIN)
void vTaskReset(void *params) {
    while (true) {
        if (xSemaphoreTake(xResetSemaphore, portMAX_DELAY) == pdTRUE) {
            if (xSemaphoreTake(xDisplayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                usuarios_ativos = 0;

                ssd1306_fill(&ssd, 0);
                ssd1306_draw_string(&ssd, "Resetado!", 5, 19);
                ssd1306_draw_string(&ssd, "Ocupado: 0", 5, 44);
                ssd1306_send_data(&ssd);

                // Alerta sonoro de reset
                pwm_set_gpio_level(BUZZER_PIN, 7812);
                vTaskDelay(pdMS_TO_TICKS(100));
                pwm_set_gpio_level(BUZZER_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                pwm_set_gpio_level(BUZZER_PIN, 7812);
                vTaskDelay(pdMS_TO_TICKS(100));
                pwm_set_gpio_level(BUZZER_PIN, 0);

                atualizar_led(0);
                xSemaphoreGive(xDisplayMutex);
            }
        }
    }
}

int main() {
    stdio_init_all();

    // Inicializa I2C e display OLED
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    ssd1306_init(&ssd, WIDTH, HEIGHT, false, ENDERECO, I2C_PORT);
    ssd1306_config(&ssd);
    ssd1306_send_data(&ssd);

    // Inicializa botões de entrada/saída/reset
    gpio_init(BOTAO_A);
    gpio_set_dir(BOTAO_A, GPIO_IN);
    gpio_pull_up(BOTAO_A);

    gpio_init(BOTAO_B);
    gpio_set_dir(BOTAO_B, GPIO_IN);
    gpio_pull_up(BOTAO_B);

    gpio_init(SW_PIN);
    gpio_set_dir(SW_PIN, GPIO_IN);
    gpio_pull_up(SW_PIN);
    gpio_set_irq_enabled(SW_PIN, GPIO_IRQ_EDGE_FALL, true);

    // Inicializa LEDs
    gpio_init(LED_RED);
    gpio_set_dir(LED_RED, GPIO_OUT);
    gpio_init(LED_GREEN);
    gpio_set_dir(LED_GREEN, GPIO_OUT);
    gpio_init(LED_BLUE);
    gpio_set_dir(LED_BLUE, GPIO_OUT);

    // Tela inicial do display
    ssd1306_fill(&ssd, 0); // Limpa display
    ssd1306_draw_string(&ssd, "Vagas: 10", 5, 20);
    ssd1306_draw_string(&ssd, "Ocupado: 0", 5, 44);
    ssd1306_send_data(&ssd);

    
    gpio_put(LED_BLUE, 1); // Azul ligado

    // Inicializa buzzer com PWM
    int buzzer_slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 4.0f);
    pwm_config_set_wrap(&cfg, 15625);
    pwm_init(buzzer_slice, &cfg, true);

    // Habilita interrupções com callback
    gpio_set_irq_enabled_with_callback(BOTAO_A, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);
    gpio_set_irq_enabled(BOTAO_B, GPIO_IRQ_EDGE_FALL, true);

    // Criação de semáforos
    xContadorEntrada = xSemaphoreCreateCounting(10, 0);
    xContadorSaida = xSemaphoreCreateCounting(10, 0);
    xResetSemaphore = xSemaphoreCreateBinary();
    xDisplayMutex = xSemaphoreCreateMutex();

    // Criação das tarefas do sistema
    xTaskCreate(vEntradaTask, "EntradaTask", configMINIMAL_STACK_SIZE + 128, NULL, 1, NULL);
    xTaskCreate(vSaidaTask, "SaidaTask", configMINIMAL_STACK_SIZE + 128, NULL, 1, NULL);
    xTaskCreate(vTaskReset, "ResetTask", configMINIMAL_STACK_SIZE + 128, NULL, 2, NULL);

    // Inicia o escalonador do FreeRTOS
    vTaskStartScheduler();

    // Caso o scheduler não inicie, entra em modo de erro
    panic_unsupported();
}
