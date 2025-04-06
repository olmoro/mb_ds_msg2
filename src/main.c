#include "project_config.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "string.h"
#include "sys/param.h"
//#include "nvs_flash.h"



static const char *TAG = "MODBUS_SLAVE";

// // Настройки UART
// #define MB_UART_NUM        UART_NUM_2
// #define MB_UART_BAUD_RATE  9600
// #define MB_UART_RXD_PIN    16
// #define MB_UART_TXD_PIN    17
// #define MB_QUEUE_SIZE      5
// #define MB_FRAME_TIMEOUT_MS 4  // 3.5 символа при 9600 бод

// Структура для передачи фреймов между задачами
typedef struct {
    uint8_t *data;
    size_t length;
} mb_frame_t;

static QueueHandle_t frame_queue = NULL;
static SemaphoreHandle_t uart_mutex = NULL;

// CRC16 для Modbus (полином 0x8005, начальное значение 0xFFFF)
uint16_t crc16_modbus(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

// Задача обработки фреймов
static void frame_processor_task(void *arg) {
    while(1) {
        mb_frame_t frame;
        if(xQueueReceive(frame_queue, &frame, portMAX_DELAY)      ) {
            // Проверка минимальной длины фрейма
            if(frame.length < 4) {
                ESP_LOGE(TAG, "Invalid frame length: %d", frame.length);
                free(frame.data);
                continue;
            }

            // Проверка CRC
            uint16_t received_crc = *(uint16_t*)&frame.data[frame.length-2];
            uint16_t calculated_crc = crc16_modbus(frame.data, frame.length-2);
            
            if(received_crc != calculated_crc) {
                ESP_LOGE(TAG, "CRC error: received 0x%04X vs calculated 0x%04X",
                        received_crc, calculated_crc);
                free(frame.data);
                continue;
            }

            ESP_LOGI(TAG, "Valid frame received (%d bytes)", frame.length);
            
            // Формирование ответа (пример)
            uint8_t response[8] = {
                frame.data[0],  // Адрес
                frame.data[1],  // Функция
                0x02,          // Длина данных
                0x01,          // Данные
                0x02,          // Данные
                0x00, 0x00     // Место для CRC
            };
            
            // Расчет CRC для ответа
            uint16_t response_crc = crc16_modbus(response, 4);
            response[5] = response_crc & 0xFF;
            response[6] = response_crc >> 8;

            // Отправка ответа с синхронизацией
            xSemaphoreTake(uart_mutex, portMAX_DELAY);
            uart_write_bytes(MB_PORT_NUM, (const char*)response, sizeof(response));
            xSemaphoreGive(uart_mutex);

            free(frame.data);
        }
    }
}

// Задача чтения UART
static void uart_reader_task(void *arg) {
    uint8_t *frame_buffer = malloc(256);
    size_t frame_index = 0;
    TickType_t last_rx_time = xTaskGetTickCount();

    while(1) {
        uint8_t buf[128];
        int len = uart_read_bytes(MB_PORT_NUM, buf, sizeof(buf), pdMS_TO_TICKS(1));
        
        if(len > 0) {
            // Копирование данных в буфер фрейма
            if(frame_index + len < 256) {
                memcpy(&frame_buffer[frame_index], buf, len);
                frame_index += len;
            } else {
                ESP_LOGE(TAG, "Frame buffer overflow");
                frame_index = 0;
            }
            last_rx_time = xTaskGetTickCount();
        }

        // Проверка таймаута фрейма
        if(frame_index > 0 && (xTaskGetTickCount() - last_rx_time) >= pdMS_TO_TICKS(MB_FRAME_TIMEOUT_MS)) {
            // Проверка минимальной длины фрейма
            if(frame_index >= 4) {
                mb_frame_t new_frame = {
                    .data = malloc(frame_index),
                    .length = frame_index
                };
                
                if(new_frame.data) {
                    memcpy(new_frame.data, frame_buffer, frame_index);
                    if(xQueueSend(frame_queue, &new_frame, pdMS_TO_TICKS(100)) != pdTRUE) {
                        ESP_LOGE(TAG, "Queue full, frame dropped");
                        free(new_frame.data);
                    }
                } else {
                    ESP_LOGE(TAG, "Memory allocation failed");
                }
            } else {
                ESP_LOGW(TAG, "Incomplete frame received");
            }
            
            frame_index = 0;
        }
    }
}

void app_main(void) {
    // // Инициализация NVS
    // esp_err_t ret = nvs_flash_init();
    // if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    //     ESP_ERROR_CHECK(nvs_flash_erase());
    //     ret = nvs_flash_init();
    // }
    // ESP_ERROR_CHECK(ret);

    // Настройка UART
    uart_config_t uart_mb_config = {
        .baud_rate = MB_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
    };

//     int intr_alloc_flags = 0;

// #if CONFIG_UART_ISR_IN_IRAM
//     intr_alloc_flags = ESP_INTR_FLAG_IRAM;
// #endif


    ESP_ERROR_CHECK(uart_driver_install(MB_PORT_NUM, BUF_SIZE, BUF_SIZE, MB_QUEUE_SIZE, NULL, 0));
    ESP_ERROR_CHECK(uart_set_pin(MB_PORT_NUM, CONFIG_MB_UART_TXD, CONFIG_MB_UART_RXD, CONFIG_MB_UART_RTS, 32));   // IO32 свободен (трюк)
    ESP_ERROR_CHECK(uart_set_mode(MB_PORT_NUM, UART_MODE_RS485_HALF_DUPLEX)); // activate RS485 half duplex in the driver
    ESP_ERROR_CHECK(uart_param_config(MB_PORT_NUM, &uart_mb_config));
    ESP_LOGI(TAG, "slave_uart initialized.");



    // Создание очереди и мьютекса
    frame_queue = xQueueCreate(MB_QUEUE_SIZE, sizeof(mb_frame_t));
    if(!frame_queue) {
        ESP_LOGE(TAG, "Queue creation failed");
        return;
    }
    
    uart_mutex = xSemaphoreCreateMutex();
    if(!uart_mutex) {
        ESP_LOGE(TAG, "Mutex creation failed");
        return;
    }

    // Создание задач
    xTaskCreate(uart_reader_task, "uart_reader", 4096, NULL, 10, NULL);
    xTaskCreate(frame_processor_task, "frame_processor", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "Modbus RTU Slave initialized");
}
