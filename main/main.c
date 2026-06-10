#include <string.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_adc/adc_continuous.h"

#define EXAMPLE_ADC_UNIT             ADC_UNIT_1
#define EXAMPLE_ADC_CONV_MODE        ADC_CONV_SINGLE_UNIT_1
#define EXAMPLE_ADC_ATTEN            ADC_ATTEN_DB_12
#define EXAMPLE_ADC_BIT_WIDTH        SOC_ADC_DIGI_MAX_BITWIDTH
#define EXAMPLE_READ_LEN            256 

static adc_channel_t channel[1] = {ADC_CHANNEL_6}; // Cambiado al GPIO3 (Seguro)

// HANDLES DE LAS TAREAS (Para Tarea_Main)
static TaskHandle_t xAdcTaskHandle = NULL;
static TaskHandle_t xTriggerTaskHandle = NULL;

// SEMÁFOROS (Los de tu diagrama)
static SemaphoreHandle_t set_trigger_event = NULL;
static SemaphoreHandle_t set_ADC_event = NULL;

// BUFFER COMPARTIDO DE MEMORIA CRUDA
#define COLA_MUESTRAS_MAX 64
static uint16_t buffer_compartido_muestras[COLA_MUESTRAS_MAX];
static uint32_t ultima_muestra_analizada = 0;

static const char *TAG_ADC = "TAREA_ADC";
static const char *TAG_TRIG = "TAREA_TRIGGER";

// ISR_DMA: Notificación directa a la Tarea_ADC
static bool IRAM_ATTR s_conv_done_cb(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    BaseType_t mustYield = pdFALSE;
    vTaskNotifyGiveFromISR(xAdcTaskHandle, &mustYield);
    return (mustYield == pdTRUE);
}

// ====================================================================
// TAREA TRIGGER (Prioridad Alta según tu diagrama)
// ====================================================================
void vTriggerTask(void *pvParameters) {
    while(1) {
        // Se queda bloqueada durmiendo hasta que Tarea_ADC le avise que el buffer se completó
        if (xSemaphoreTake(set_trigger_event, portMAX_DELAY) == pdTRUE) {
            
            // Analiza el bloque de muestras guardado en la memoria compartida
            for (int i = 0; i < COLA_MUESTRAS_MAX; i++) {
                uint32_t muestra_actual = buffer_compartido_muestras[i];

                // BUSCA CRUCE DE NIVEL CONFIGURADO (Ej: Umbral en 2048 - Flanco de Subida)
                if (muestra_actual > 2048 && ultima_muestra_analizada <= 2048) {
                    ESP_LOGI(TAG_TRIG, "¡Condición de Trigger Detectada! Valor: %"PRIu32, muestra_actual);
                    
                    // Notifica a Tarea_ADC que decida capturar la ventana
                    xSemaphoreGive(set_ADC_event);
                    break; // Salimos para evitar múltiples disparos en el mismo paquete
                }
                ultima_muestra_analizada = muestra_actual;
            }
        }
    }
}

// ====================================================================
// TAREA ADC (Manejo de Adquisición y DMA)
// ====================================================================
static void continuous_adc_init(adc_channel_t *channel, uint8_t channel_num, adc_continuous_handle_t *out_handle);

void vAdcTask(void *pvParameters) {
    esp_err_t ret;
    uint32_t ret_num = 0;
    uint8_t result[EXAMPLE_READ_LEN] = {0};
    bool capturando_ventana = false;

    adc_continuous_handle_t handle = NULL;
    continuous_adc_init(channel, 1, &handle); 

    adc_continuous_evt_cbs_t cbs = { .on_conv_done = s_conv_done_cb };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(handle, &cbs, NULL));
    ESP_ERROR_CHECK(adc_continuous_start(handle));

    while (1) {
        // Espera el evento bin_dma_event de la ISR
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (1) {
            ret = adc_continuous_read(handle, result, EXAMPLE_READ_LEN, &ret_num, 0);
            
            if (ret == ESP_OK) {
                uint32_t samples_count = ret_num / SOC_ADC_DIGI_RESULT_BYTES;
                adc_continuous_data_t parsed_data[samples_count];
                uint32_t num_parsed_samples = 0;

                esp_err_t parse_ret = adc_continuous_parse_data(handle, result, ret_num, parsed_data, &num_parsed_samples);
                
                if (parse_ret == ESP_OK) {
                    // Copiamos los datos válidos al buffer compartido para que los vea la Tarea_Trigger
                    for (int i = 0; i < num_parsed_samples && i < COLA_MUESTRAS_MAX; i++) {
                        if (parsed_data[i].valid) {
                            buffer_compartido_muestras[i] = parsed_data[i].raw_data;
                        }
                    }

                    // NOTIFICA A TAREA_TRIGGER QUE EL MUESTREO SE COMPLETÓ (Semaforo)
                    xSemaphoreGive(set_trigger_event);

                    // REVISAMOS SI LA TAREA_TRIGGER NOS ORDENÓ CAPTURAR UNA VENTANA
                    if (xSemaphoreTake(set_ADC_event, 0) == pdTRUE) {
                        capturando_ventana = true;
                    }

                    if (capturando_ventana) {
                        ESP_LOGW(TAG_ADC, "Enviando ventana de datos calificados a q_digsig...");
                        // TODO: xQueueSend(q_digsig, &buffer_compartido_muestras, 0);
                        capturando_ventana = false; // Reset de captura por ahora
                    }
                }
            } else if (ret == ESP_ERR_TIMEOUT) {
                break;
            }
        }
    }
}

// ====================================================================
// TAREA MAIN (Inicializadora)
// ====================================================================
void app_main(void)
{
    // Crear Semáforos binarios limpios
    set_trigger_event = xSemaphoreCreateBinary();
    set_ADC_event = xSemaphoreCreateBinary();

    // Crear Tareas independientes (Asignando prioridades altas como tu esquema)
    xTaskCreate(vAdcTask, "Tarea_ADC", 4096, NULL, 4, &xAdcTaskHandle);
    xTaskCreate(vTriggerTask, "Tarea_Trigger", 4096, NULL, 4, &xTriggerTaskHandle);
}

// Configuración Base del ADC (Frecuencia fijada a 1 kHz para estabilidad UART)
static void continuous_adc_init(adc_channel_t *channel, uint8_t channel_num, adc_continuous_handle_t *out_handle) {
    adc_continuous_handle_t handle = NULL;
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 1024,
        .conv_frame_size = EXAMPLE_READ_LEN,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &handle));

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = 1 * 1000, 
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