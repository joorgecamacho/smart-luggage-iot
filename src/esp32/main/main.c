/**
 * @file main.c
 * @brief AeroTrack IoT - Real-Time Baggage Tracking & Hold Verification System
 * @target ESP32-S2 DevKitM-1 (ESP-IDF v6.x / FreeRTOS)
 * 
 * Hardware Pinout Configuration:
 *   - Indicators:
 *       Green LED: GPIO 16 (Hold load confirmation)
 *       Red LED:   GPIO 17 (Misrouted security alert)
 *   - Physical Push-Buttons (Internal Pull-Up):
 *       Button 1 (Valid Luggage):   GPIO 4 -> GND
 *       Button 2 (Misrouted Bag):   GPIO 5 -> GND
 *   - Analog Inputs (ADC1 OneShot):
 *       Potentiometer 1 (Conveyor Speed): GPIO 1 (ADC1_CHANNEL_0)
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

// =======================================================
// HARDWARE PIN DEFINITIONS (ESP32-S2)
// =======================================================
#define PIN_LED_ALERT_RED       17   // Red LED (Fila 3 - Pin 17)
#define PIN_LED_VALID_GREEN     16   // Green LED (Fila 4 - Pin 16)

#define PIN_BTN_ROGUE           5    // Misrouted Bag Button (Fila 15 - Pin 5)
#define PIN_BTN_VALID           4    // Valid Bag Button (Fila 16 - Pin 4)

#define ADC_CH_BELT_SPEED       ADC_CHANNEL_0  // Conveyor Speed (Fila 19 - Pin 1 / ADC1_CH0)

// Global Driver Handles
static adc_oneshot_unit_handle_t adc1_handle;

// Luggage Event Data Structure
typedef enum {
    SCAN_STATUS_VALID,
    SCAN_STATUS_ROGUE
} scan_status_t;

typedef struct {
    char tag_uid[32];
    scan_status_t status;
    int conveyor_speed_pct;
} luggage_event_t;

// FreeRTOS Inter-Task Communication
static QueueHandle_t xQueueLuggageEvent = NULL;
static int g_hold_loaded_count = 198;
static const int g_flight_total_expected = 200;

// Helper: Dispatch luggage event to the processing queue
static void dispatch_tag_event(const char *uid_str, scan_status_t status)
{
    int raw_speed = 0;
    adc_oneshot_read(adc1_handle, ADC_CH_BELT_SPEED, &raw_speed);

    luggage_event_t event;
    strncpy(event.tag_uid, uid_str, sizeof(event.tag_uid) - 1);
    event.tag_uid[sizeof(event.tag_uid) - 1] = '\0';
    event.status = status;
    event.conveyor_speed_pct = (raw_speed * 100) / 8191;

    xQueueSend(xQueueLuggageEvent, &event, portMAX_DELAY);
}

// =======================================================
// FREERTOS TASKS
// =======================================================

/**
 * @brief Task 1: Periodic Sensor Telemetry & Button Debug Monitor
 * Streams conveyor speed and raw button states every 2 seconds.
 */
void task_telemetry(void *pvParameters)
{
    int raw_speed = 0;

    while (1) {
        adc_oneshot_read(adc1_handle, ADC_CH_BELT_SPEED, &raw_speed);
        int speed_pct = (raw_speed * 100) / 8191;

        // Read physical button states (1 = released, 0 = pressed to GND)
        int btn_val = gpio_get_level(PIN_BTN_VALID);
        int btn_rog = gpio_get_level(PIN_BTN_ROGUE);

        printf("[TELEMETRY] Belt: %d%% | Hold: %d/%d bags | Btn_Valid(Pin4): %s | Btn_Rogue(Pin5): %s\n", 
               speed_pct, g_hold_loaded_count, g_flight_total_expected,
               (btn_val == 0) ? "PRESSED [OK]" : "idle (1)",
               (btn_rog == 0) ? "PRESSED [OK]" : "idle (1)");

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/**
 * @brief Task 2: Physical Push-Button Scanner
 * Polls GPIO 4 (Valid Bag) and GPIO 5 (Misrouted Bag) with 300ms software debounce.
 */
void task_buttons(void *pvParameters)
{
    TickType_t last_press_time = 0;
    const TickType_t debounce_ticks = pdMS_TO_TICKS(300);

    while (1) {
        TickType_t now = xTaskGetTickCount();

        // Check Button 1 (Valid Luggage - Pin 4)
        if (gpio_get_level(PIN_BTN_VALID) == 0 && (now - last_press_time > debounce_ticks)) {
            last_press_time = now;
            printf("\n>>> [HARDWARE BUTTON] Valid Luggage Pressed! (GPIO %d)\n", PIN_BTN_VALID);
            dispatch_tag_event("11-22-33-44", SCAN_STATUS_VALID);
        }

        // Check Button 2 (Misrouted Luggage - Pin 5)
        if (gpio_get_level(PIN_BTN_ROGUE) == 0 && (now - last_press_time > debounce_ticks)) {
            last_press_time = now;
            printf("\n>>> [HARDWARE BUTTON] Misrouted Luggage Pressed! (GPIO %d)\n", PIN_BTN_ROGUE);
            dispatch_tag_event("55-66-77-88", SCAN_STATUS_ROGUE);
        }

        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/**
 * @brief Task 3: Interactive Terminal Fallback
 */
void task_terminal(void *pvParameters)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    while (1) {
        char ch = 0;
        int n = read(STDIN_FILENO, &ch, 1);
        if (n > 0) {
            if (ch == '1' || ch == 'g' || ch == 'G') {
                printf("\n>>> [KEYBOARD] Triggered Valid Luggage (Tag 11-22-33-44)\n");
                dispatch_tag_event("11-22-33-44", SCAN_STATUS_VALID);
            } else if (ch == '2' || ch == 'y' || ch == 'Y') {
                printf("\n>>> [KEYBOARD] Triggered Misrouted Luggage (Tag 55-66-77-88)\n");
                dispatch_tag_event("55-66-77-88", SCAN_STATUS_ROGUE);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * @brief Task 4: Cloud MQTT Publisher & Safety Interlock Manager
 */
void task_cloud_MQTT(void *pvParameters)
{
    luggage_event_t event;

    while (1) {
        if (xQueueReceive(xQueueLuggageEvent, &event, portMAX_DELAY) == pdTRUE) {
            
            if (event.status == SCAN_STATUS_VALID) {
                g_hold_loaded_count++;
                
                // Green LED ON for confirmation
                gpio_set_level(PIN_LED_VALID_GREEN, 1);
                
                printf("\n=======================================================\n");
                printf(">>> [MQTT PUB] Topic: aerotrack/esp32/luggage/scan\n");
                printf("{\n");
                printf("  \"flight_id\": \"IB3166\",\n");
                printf("  \"tag_uid\": \"%s\",\n", event.tag_uid);
                printf("  \"status\": \"LOADED_CONFIRMED\",\n");
                printf("  \"hold_tally\": \"%d/%d\",\n", g_hold_loaded_count, g_flight_total_expected);
                printf("  \"conveyor_speed_pct\": %d,\n", event.conveyor_speed_pct);
                printf("  \"passenger_notification\": \"Luggage loaded into hold successfully\"\n");
                printf("}\n");
                printf("=======================================================\n\n");

                vTaskDelay(pdMS_TO_TICKS(1000));
                gpio_set_level(PIN_LED_VALID_GREEN, 0);

            } else {
                // Misrouted baggage security alert
                printf("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
                printf(">>> [MQTT ALERT] Topic: aerotrack/esp32/alerts\n");
                printf("{\n");
                printf("  \"flight_id\": \"IB3166\",\n");
                printf("  \"tag_uid\": \"%s\",\n", event.tag_uid);
                printf("  \"status\": \"ALERT_MISROUTED\",\n");
                printf("  \"warning\": \"LUGGAGE NOT ASSIGNED TO THIS FLIGHT (Destination Berlin)\",\n");
                printf("  \"hold_action\": \"HALT_CONVEYOR\",\n");
                printf("  \"conveyor_speed_pct\": %d\n", event.conveyor_speed_pct);
                printf("}\n");
                printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n");

                // Red LED rapid alert flashing (6 cycles)
                for (int i = 0; i < 6; i++) {
                    gpio_set_level(PIN_LED_ALERT_RED, 1);
                    vTaskDelay(pdMS_TO_TICKS(150));
                    gpio_set_level(PIN_LED_ALERT_RED, 0);
                    vTaskDelay(pdMS_TO_TICKS(150));
                }
            }
        }
    }
}

// =======================================================
// SYSTEM INITIALIZATION & MAIN ENTRY POINT
// =======================================================
void app_main(void)
{
    printf("\n=======================================================\n");
    printf("   AEROTRACK IoT - ESP32-S2 FIRMWARE RUNNING\n");
    printf("=======================================================\n");
    printf("Pins Configured:\n");
    printf("  - Potentiometer: Pin 1  (Row 19)\n");
    printf("  - Button Valid:  Pin 4  (Row 16)\n");
    printf("  - Button Rogue:  Pin 5  (Row 15)\n");
    printf("  - LED Green:     Pin 16 (Row 4)\n");
    printf("  - LED Red:       Pin 17 (Row 3)\n");
    printf("=======================================================\n\n");

    // 1. Configure Status Indicator LEDs (Outputs)
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << PIN_LED_VALID_GREEN) | (1ULL << PIN_LED_ALERT_RED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);

    // Boot LED Self-Test: Flash 3 times rapidly
    printf("[BOOT TEST] Flashing LEDs 3 times...\n");
    for (int i = 0; i < 3; i++) {
        gpio_set_level(PIN_LED_VALID_GREEN, 1);
        gpio_set_level(PIN_LED_ALERT_RED, 1);
        vTaskDelay(pdMS_TO_TICKS(250));
        gpio_set_level(PIN_LED_VALID_GREEN, 0);
        gpio_set_level(PIN_LED_ALERT_RED, 0);
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    // 2. Configure Physical Push-Buttons (Inputs with Internal Pull-Ups)
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << PIN_BTN_VALID) | (1ULL << PIN_BTN_ROGUE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

    // 3. Configure ADC1 for Single Conveyor Speed Potentiometer (GPIO 1)
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CH_BELT_SPEED, &config));

    // 4. Create FreeRTOS Event Queue
    xQueueLuggageEvent = xQueueCreate(10, sizeof(luggage_event_t));

    // 5. Launch FreeRTOS Tasks
    xTaskCreate(task_telemetry,  "task_telemetry",  4096, NULL, 3, NULL);
    xTaskCreate(task_buttons,    "task_buttons",    2048, NULL, 5, NULL);
    xTaskCreate(task_terminal,   "task_terminal",   2048, NULL, 4, NULL);
    xTaskCreate(task_cloud_MQTT, "task_cloud_MQTT", 4096, NULL, 4, NULL);

    printf("AeroTrack IoT System Ready.\n\n");
}
