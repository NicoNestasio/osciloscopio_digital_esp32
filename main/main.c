#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_adc/adc_continuous.h"
#include "driver/ledc.h"
#include "driver/uart.h"

// ====================================================================
// CONFIGURACIONES GENERALES DEL PERIFÉRICO
// ====================================================================
#define EXAMPLE_ADC_UNIT             ADC_UNIT_1
#define EXAMPLE_ADC_CONV_MODE        ADC_CONV_SINGLE_UNIT_1
#define EXAMPLE_ADC_ATTEN            ADC_ATTEN_DB_12
#define EXAMPLE_ADC_BIT_WIDTH        SOC_ADC_DIGI_MAX_BITWIDTH

#define EXAMPLE_READ_LEN             4096 
#define NUM_MUESTRAS                 2048
#define PUNTOS_PANTALLA              400 

static adc_channel_t channel[1] = {ADC_CHANNEL_6}; 
static TaskHandle_t xAdcTaskHandle = NULL;
static TaskHandle_t xTriggerTaskHandle = NULL;

// Usaremos una sola cola para pasar los punteros de memoria
static QueueHandle_t cola_llenos = NULL;

// ====================================================================
// VARIABLES GLOBALES (Protección contra Stack Overflow y Watchdog)
// ====================================================================
static uint8_t hardware_read_buffer[EXAMPLE_READ_LEN];
static adc_continuous_data_t parsed_data_buffer[EXAMPLE_READ_LEN / SOC_ADC_DIGI_RESULT_BYTES];

// Doble buffer Ping-Pong estático en RAM global
static uint16_t buffer_A[NUM_MUESTRAS];
static uint16_t buffer_B[NUM_MUESTRAS];

// Parámetros del Trigger
volatile uint32_t nivel_trigger = 2000;
#define HISTERESIS 100 // Filtro de ruido para los flancos

typedef enum { FLANCO_ASCENDENTE = 0, FLANCO_DESCENDENTE = 1 } tipo_flanco_t;
volatile tipo_flanco_t flanco_trigger = FLANCO_DESCENDENTE; 

// ====================================================================
// GENERADOR DE SEÑAL DE PRUEBA (PWM a 50Hz)
// ====================================================================
#define PWM_OUTPUT_GPIO    5   
#define PWM_FREQ_HZ        50  

static void init_pwm_test_signal(void) {
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_10_BIT, 
        .freq_hz          = PWM_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = PWM_OUTPUT_GPIO,
        .duty           = 512, 
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

static void continuous_adc_init(adc_channel_t *channel, uint8_t channel_num, adc_continuous_handle_t *out_handle);

static bool IRAM_ATTR s_conv_done_cb(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data) {
    BaseType_t mustYield = pdFALSE;
    vTaskNotifyGiveFromISR(xAdcTaskHandle, &mustYield);
    return (mustYield == pdTRUE);
}

// ====================================================================
// TAREA 2: TRIGGER Y UART (El Consumidor)
// ====================================================================
void vTriggerTask(void *pvParameters) {
    uint16_t* buffer_a_procesar;
    TickType_t ultimo_dibujo = 0;

    while(1) {
        // La tarea duerme profundamente hasta recibir un puntero del ADC
        if (xQueueReceive(cola_llenos, &buffer_a_procesar, portMAX_DELAY) == pdTRUE) {
            
            TickType_t ahora = xTaskGetTickCount();
            
            // Limitamos la transmisión UART para no colapsar la conexión
            if ((ahora - ultimo_dibujo) > pdMS_TO_TICKS(150)) {
                
                int indice_del_disparo = -1;
                
                // 1. ZONA SEGURA: Buscamos solo donde los 400 puntos entran perfectos
                // Evitamos mirar los bordes del buffer para que la ventana nunca se recorte.
                int inicio_busqueda = PUNTOS_PANTALLA / 2;
                int fin_busqueda = NUM_MUESTRAS - (PUNTOS_PANTALLA / 2);

                for (int i = inicio_busqueda; i < fin_busqueda; i++) {
                    uint32_t anterior = buffer_a_procesar[i - 1];
                    uint32_t actual   = buffer_a_procesar[i];

                    if (flanco_trigger == FLANCO_ASCENDENTE) {
                        if (anterior < (nivel_trigger - HISTERESIS) && actual >= (nivel_trigger + HISTERESIS)) {
                            indice_del_disparo = i;
                            break; // Primer flanco encontrado en zona segura
                        }
                    } else {
                        if (anterior > (nivel_trigger + HISTERESIS) && actual <= (nivel_trigger - HISTERESIS)) {
                            indice_del_disparo = i;
                            break; // Primer flanco encontrado en zona segura
                        }
                    }
                }

                // =======================================================
                // ¡NUEVO! MODO AUTO (Auto-Trigger Fallback)
                // Si revisó todo el buffer y no hubo cruce con el nivel,
                // forzamos el índice al centro para actualizar la pantalla.
                // =======================================================
                if (indice_del_disparo == -1) {
                    indice_del_disparo = NUM_MUESTRAS / 2;
                }

                // 2. ENCUADRE Y TRANSMISIÓN
                // Como tenemos la Zona Segura o el Modo Auto, esta matemática es 100% a prueba de fallos.
                int inicio = indice_del_disparo - (PUNTOS_PANTALLA / 2);
                int fin = indice_del_disparo + (PUNTOS_PANTALLA / 2);

                // Imprimimos la ventana estricta de 400 puntos
                for (int i = inicio; i < fin; i++) {
                    printf("%"PRIu32"\n", (uint32_t)buffer_a_procesar[i]);
                }
                
                ultimo_dibujo = xTaskGetTickCount(); // Actualizamos el reloj
            }
        }
    }
}

// ====================================================================
// TAREA 1: ADC (El Productor Ininterrumpido)
// ====================================================================
void vAdcTask(void *pvParameters) {
    esp_err_t ret;
    uint32_t ret_num = 0;
    
    uint16_t* buffer_activo = buffer_A; 
    int muestras_acumuladas = 0;

    adc_continuous_handle_t handle = NULL;
    continuous_adc_init(channel, 1, &handle); 

    adc_continuous_evt_cbs_t cbs = { .on_conv_done = s_conv_done_cb };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(handle, &cbs, NULL));
    ESP_ERROR_CHECK(adc_continuous_start(handle));

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (1) {
            ret = adc_continuous_read(handle, hardware_read_buffer, EXAMPLE_READ_LEN, &ret_num, 0);
            
            if (ret == ESP_OK) {
                uint32_t num_parsed_samples = 0;
                esp_err_t parse_ret = adc_continuous_parse_data(handle, hardware_read_buffer, ret_num, parsed_data_buffer, &num_parsed_samples);
                
                if (parse_ret == ESP_OK) {
                    for (int i = 0; i < num_parsed_samples; i++) {
                        if (parsed_data_buffer[i].valid) {
                            
                            buffer_activo[muestras_acumuladas] = parsed_data_buffer[i].raw_data;
                            muestras_acumuladas++;

                            if (muestras_acumuladas >= NUM_MUESTRAS) {
                                
                                // Mandamos el buffer a la cola (tiempo de espera 0)
                                if (xQueueSend(cola_llenos, &buffer_activo, 0) == pdPASS) {
                                    // Cambiamos de buffer (Ping-Pong)
                                    buffer_activo = (buffer_activo == buffer_A) ? buffer_B : buffer_A;
                                }
                                
                                // Reseteamos contador. Si el Trigger estaba ocupado, se sobreescribe pacíficamente.
                                muestras_acumuladas = 0;
                            }
                        }
                    }
                }
            } else if (ret == ESP_ERR_TIMEOUT) {
                break;
            }
        }
    }
}

// ====================================================================
// TAREA MAIN 
// ====================================================================
void app_main(void) {
    init_pwm_test_signal();
    uart_set_baudrate(UART_NUM_0, 921600);

    // Cola con capacidad para 1 solo puntero. Actúa como sincronizador no bloqueante.
    cola_llenos = xQueueCreate(1, sizeof(uint16_t*));

    xTaskCreatePinnedToCore(vAdcTask, "Tarea_ADC", 8192, NULL, 4, &xAdcTaskHandle, 0);
    xTaskCreatePinnedToCore(vTriggerTask, "Tarea_Trigger", 8192, NULL, 4, &xTriggerTaskHandle, 1);
}

// ====================================================================
// INICIALIZACIÓN DEL ADC
// ====================================================================
static void continuous_adc_init(adc_channel_t *channel, uint8_t channel_num, adc_continuous_handle_t *out_handle) {
    adc_continuous_handle_t handle = NULL;
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 8192,
        .conv_frame_size = EXAMPLE_READ_LEN,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &handle));

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = 20000, 
        .conv_mode = EXAMPLE_ADC_CONV_MODE,
    };
    
    adc_digi_pattern_config_t adc_pattern[1] = {0};
    dig_cfg.pattern_num = channel_num;
    adc_pattern[0].atten = EXAMPLE_ADC_ATTEN;
    adc_pattern[0].channel = channel[0] & 0x7;
    adc_pattern[0].unit = EXAMPLE_ADC_UNIT;
    adc_pattern[0].bit_width = EXAMPLE_ADC_BIT_WIDTH;
    dig_cfg.adc_pattern = adc_pattern;
    ESP_ERROR_CHECK(adc_continuous_config(handle, &dig_cfg));
    *out_handle = handle;
}