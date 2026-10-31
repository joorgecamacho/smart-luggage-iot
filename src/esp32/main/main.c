/**
 * @file main.c
 * @brief AeroTrack IoT - Real-Time Baggage Tracking & Hold Verification System
 * @target ESP32-S2 DevKitM-1 (ESP-IDF v5.x / FreeRTOS)
 * 
 * Hardware Pinout Configuration:
 *   - RFID Reader (MFRC522 over SPI2 native bus):
 *       RST:  GPIO 33
 *       SDA:  GPIO 34 (CS / SS)
 *       MOSI: GPIO 35
 *       SCK:  GPIO 36
 *       MISO: GPIO 37
 *   - Indicators:
 *       Green LED: GPIO 16 (Hold load confirmation)
 *       Red LED:   GPIO 17 (Misrouted security alert)
 *   - Physical Push-Buttons (Internal Pull-Up):
 *       Button 1 (Valid Luggage):   GPIO 4 -> GND
 *       Button 2 (Misrouted Bag):   GPIO 5 -> GND
 *   - Analog Inputs (ADC1 OneShot):
 *       Potentiometer 1 (Conveyor Speed): GPIO 1 (ADC1_CHANNEL_0)
 *       Potentiometer 2 (Cargo Door):     GPIO 2 (ADC1_CHANNEL_1)
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
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"

// =======================================================
// HARDWARE PIN DEFINITIONS (ESP32-S2 DevKitM-1)
// =======================================================
// Native SPI2 bus for MFRC522 (Flash-safe GPIOs)
#define PIN_RC522_RST           33   // Reset line
#define PIN_RC522_SDA           34   // Chip Select (CS / SS)
#define PIN_RC522_MOSI          35   // Master Out Slave In
#define PIN_RC522_SCK           36   // Serial Clock
#define PIN_RC522_MISO          37   // Master In Slave Out

// Status Indicator LEDs
#define PIN_LED_VALID_GREEN     16   // Green LED: Luggage load confirmed
#define PIN_LED_ALERT_RED       17   // Red LED: Misrouted baggage alarm

// Physical Push-Buttons (with internal pull-up resistors)
#define PIN_BTN_VALID           4    // Push-button: Valid baggage (Flight IB3166)
#define PIN_BTN_ROGUE           5    // Push-button: Misrouted baggage (Destination Berlin)

// ADC1 Channels (Potentiometers)
#define ADC_CH_BELT_SPEED       ADC_CHANNEL_0  // GPIO 1: Conveyor belt speed (0-100%)
#define ADC_CH_DOOR_ANGLE       ADC_CHANNEL_1  // GPIO 2: Cargo hold door sensor (>70% = closed)

// =======================================================
// MFRC522 REGISTERS & ISO 14443A CONSTANTS
// =======================================================
#define CommandReg              0x01
#define ComIEnReg               0x02
#define ComIrqReg               0x04
#define ErrorReg                0x06
#define FIFODataReg             0x09
#define FIFOLevelReg            0x0A
#define ControlReg              0x0C
#define BitFramingReg           0x0D
#define ModeReg                 0x11
#define TxControlReg            0x14
#define TxASKReg                0x15
#define TxModeReg               0x12
#define RxModeReg               0x13
#define ModWidthReg             0x24
#define TModeReg                0x2A
#define TPrescalerReg           0x2B
#define TReloadRegH             0x2C
#define TReloadRegL             0x2D
#define VersionReg              0x37

// PCD and PICC Command Codes
#define PCD_IDLE                0x00
#define PCD_TRANSCEIVE          0x0C
#define PCD_RESETPHASE          0x0F
#define PICC_CMD_REQA           0x26
#define PICC_CMD_WUPA           0x52
#define PICC_CMD_SEL_CL1        0x93
#define PICC_CMD_HLTA           0x50

#define MI_OK                   0
#define MI_NOTAGERR             1
#define MI_ERR                  2

// Global Driver Handles
static spi_device_handle_t spi_rfid_handle;
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
    int door_angle_pct;
    bool is_door_closed;
} luggage_event_t;

// FreeRTOS Inter-Task Communication
static QueueHandle_t xQueueLuggageEvent = NULL;
static int g_hold_loaded_count = 198;
static const int g_flight_total_expected = 200;

// =======================================================
// LOW-LEVEL MFRC522 SPI FUNCTIONS
// =======================================================
static void mfrc522_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx_data[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx_data,
    };
    spi_device_polling_transmit(spi_rfid_handle, &t);
}

static uint8_t mfrc522_read_reg(uint8_t reg)
{
    uint8_t tx_data[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00 };
    uint8_t rx_data[2] = { 0 };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };
    spi_device_polling_transmit(spi_rfid_handle, &t);
    return rx_data[1];
}

static void mfrc522_init(void)
{
    // 1. Hardware reset using RST pin (GPIO 33)
    gpio_set_direction(PIN_RC522_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_RC522_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RC522_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // 2. Soft reset via SPI command
    mfrc522_write_reg(CommandReg, PCD_RESETPHASE);
    vTaskDelay(pdMS_TO_TICKS(50));

    // 3. Configure ISO 14443A standard RF timeout (25 ms response window)
    mfrc522_write_reg(TModeReg, 0x80);      // TAuto=1: timer starts automatically at end of transmission
    mfrc522_write_reg(TPrescalerReg, 0xA9); // 40 kHz timer frequency (25 us period)
    mfrc522_write_reg(TReloadRegH, 0x03);   // 0x03E8 = 1000 ticks = 25 ms
    mfrc522_write_reg(TReloadRegL, 0xE8);

    // 4. 100% ASK modulation & CRC presets
    mfrc522_write_reg(TxASKReg, 0x40);
    mfrc522_write_reg(ModeReg, 0x3D);

    // 5. Enable RF transmitter antennas Tx1 and Tx2
    uint8_t tx_ctrl = mfrc522_read_reg(TxControlReg);
    if ((tx_ctrl & 0x03) != 0x03) {
        mfrc522_write_reg(TxControlReg, tx_ctrl | 0x03);
    }
}

static uint8_t mfrc522_to_card(uint8_t command, uint8_t *sendData, uint8_t sendLen,
                               uint8_t *backData, uint8_t *backLen, uint8_t validBits)
{
    uint8_t status = MI_ERR;
    uint8_t waitIRq = 0x30; // RxIRq and IdleIRq

    mfrc522_write_reg(CommandReg, PCD_IDLE);
    mfrc522_write_reg(ComIrqReg, 0x7F);      // Clear all 7 interrupt bits
    mfrc522_write_reg(FIFOLevelReg, 0x80);   // Flush FIFO buffer

    for (uint8_t i = 0; i < sendLen; i++) {
        mfrc522_write_reg(FIFODataReg, sendData[i]);
    }

    mfrc522_write_reg(BitFramingReg, validBits & 0x07);
    mfrc522_write_reg(CommandReg, command);

    if (command == PCD_TRANSCEIVE) {
        uint8_t cur = mfrc522_read_reg(BitFramingReg);
        mfrc522_write_reg(BitFramingReg, cur | 0x80); // StartSend = 1
    }

    // Wait for response with timeout safety margin
    uint32_t i = 25000;
    while (1) {
        uint8_t n = mfrc522_read_reg(ComIrqReg);
        uint8_t fifo_level = mfrc522_read_reg(FIFOLevelReg);

        if ((n & waitIRq) || (fifo_level > 0)) {
            status = MI_OK;
            break;
        }
        if (n & 0x01) { // Timer timeout without PICC response
            status = MI_NOTAGERR;
            break;
        }
        if (--i == 0) {
            status = MI_ERR;
            break;
        }
    }

    // Clear StartSend bit
    uint8_t cur = mfrc522_read_reg(BitFramingReg);
    mfrc522_write_reg(BitFramingReg, cur & (~0x80));

    if (status == MI_OK) {
        uint8_t err = mfrc522_read_reg(ErrorReg);
        if (err & 0x13) {
            return MI_ERR;
        }

        if (backData && backLen) {
            uint8_t fifo_len = mfrc522_read_reg(FIFOLevelReg);
            if (fifo_len > *backLen) {
                fifo_len = *backLen;
            }
            *backLen = fifo_len;
            for (uint8_t j = 0; j < fifo_len; j++) {
                backData[j] = mfrc522_read_reg(FIFODataReg);
            }
        }
    }
    return status;
}

static uint8_t mfrc522_request_cmd(uint8_t reqMode, uint8_t *tagType)
{
    uint8_t backLen = 2;
    tagType[0] = reqMode;
    mfrc522_write_reg(TxModeReg, 0x00);
    mfrc522_write_reg(RxModeReg, 0x00);
    mfrc522_write_reg(ModWidthReg, 0x26);

    uint8_t status = mfrc522_to_card(PCD_TRANSCEIVE, tagType, 1, tagType, &backLen, 7);
    if (status == MI_OK && backLen >= 2) {
        return MI_OK;
    }
    return MI_ERR;
}

static uint8_t mfrc522_request(uint8_t *tagType)
{
    if (mfrc522_request_cmd(PICC_CMD_REQA, tagType) == MI_OK) {
        return MI_OK;
    }
    return mfrc522_request_cmd(PICC_CMD_WUPA, tagType);
}

static uint8_t mfrc522_anticoll(uint8_t *serNum)
{
    uint8_t cmd[2] = { PICC_CMD_SEL_CL1, 0x20 };
    uint8_t backLen = 5;

    uint8_t status = mfrc522_to_card(PCD_TRANSCEIVE, cmd, 2, serNum, &backLen, 0);
    if (status == MI_OK && backLen >= 5) {
        uint8_t bcc = serNum[0] ^ serNum[1] ^ serNum[2] ^ serNum[3];
        if (bcc == serNum[4]) {
            return MI_OK;
        }
    }
    return MI_ERR;
}

static void mfrc522_halt(void)
{
    uint8_t cmd[4] = { PICC_CMD_HLTA, 0x00, 0x00, 0x00 };
    uint8_t backLen = 0;
    mfrc522_to_card(PCD_TRANSCEIVE, cmd, 2, NULL, &backLen, 0);
}

// Print welcome and instruction banner
static void print_instructions_banner(void)
{
    printf("\n=======================================================\n");
    printf("   AEROTRACK IoT - BAGGAGE TRACKING SYSTEM (ESP32-S2)\n");
    printf("=======================================================\n");
    printf("Hardware Inputs Available:\n");
    printf("  - Button 1 (GPIO 4):  Valid Baggage   (Flight IB3166)\n");
    printf("  - Button 2 (GPIO 5):  Misrouted Bag   (Destination Berlin)\n");
    printf("  - Potentiometer P1:   Conveyor speed  (GPIO 1 / ADC1_CH0)\n");
    printf("  - Potentiometer P2:   Cargo door angle(GPIO 2 / ADC1_CH1)\n");
    printf("  - MFRC522 RFID:       Native SPI2 bus (GPIOs 33-37)\n");
    printf("-------------------------------------------------------\n");
    printf("Serial Keyboard Fallback (Press key + Enter):\n");
    printf("  [1] or [g] -> Valid Baggage   (Tag: 11-22-33-44)\n");
    printf("  [2] or [y] -> Misrouted Alert (Tag: 55-66-77-88)\n");
    printf("  [3] or [b] -> Valid Baggage   (Tag: 01-02-03-04)\n");
    printf("  [h]       -> Show this help banner\n");
    printf("=======================================================\n\n");
}

// Helper: Dispatch luggage event to the processing queue
static void dispatch_tag_event(const char *uid_str, scan_status_t status)
{
    int raw_speed = 0, raw_door = 0;
    adc_oneshot_read(adc1_handle, ADC_CH_BELT_SPEED, &raw_speed);
    adc_oneshot_read(adc1_handle, ADC_CH_DOOR_ANGLE, &raw_door);

    luggage_event_t event;
    strncpy(event.tag_uid, uid_str, sizeof(event.tag_uid) - 1);
    event.tag_uid[sizeof(event.tag_uid) - 1] = '\0';
    event.status = status;
    event.conveyor_speed_pct = (raw_speed * 100) / 8191;
    event.door_angle_pct     = (raw_door * 100) / 8191;
    event.is_door_closed     = (event.door_angle_pct > 70);

    xQueueSend(xQueueLuggageEvent, &event, portMAX_DELAY);
}

// =======================================================
// FREERTOS TASKS
// =======================================================

/**
 * @brief Task 1: Periodic Sensor Telemetry
 * Reads conveyor speed and cargo door sensors via ADC1 every 3 seconds.
 */
void task_telemetry(void *pvParameters)
{
    int raw_speed = 0, raw_door = 0;

    while (1) {
        adc_oneshot_read(adc1_handle, ADC_CH_BELT_SPEED, &raw_speed);
        adc_oneshot_read(adc1_handle, ADC_CH_DOOR_ANGLE, &raw_door);

        int speed_pct = (raw_speed * 100) / 8191;
        int door_pct  = (raw_door * 100) / 8191;
        const char *door_state = (door_pct > 70) ? "CLOSED" : "OPEN";

        printf("[TELEMETRY] Conveyor belt: %d%% | Cargo hold: %d/%d bags | Door: %d%% (%s)\n", 
               speed_pct, g_hold_loaded_count, g_flight_total_expected, door_pct, door_state);

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/**
 * @brief Task 2: Physical Push-Button Scanner
 * Polls GPIO 4 (Valid Bag) and GPIO 5 (Misrouted Bag) with 400ms software debounce.
 */
void task_buttons(void *pvParameters)
{
    TickType_t last_press_time = 0;
    const TickType_t debounce_ticks = pdMS_TO_TICKS(400);

    while (1) {
        TickType_t now = xTaskGetTickCount();

        // Check Button 1 (Valid Luggage - Flight IB3166)
        if (gpio_get_level(PIN_BTN_VALID) == 0 && (now - last_press_time > debounce_ticks)) {
            last_press_time = now;
            printf("\n>>> [BUTTON] Triggered: Valid Baggage Push-Button (GPIO %d)\n", PIN_BTN_VALID);
            dispatch_tag_event("11-22-33-44", SCAN_STATUS_VALID);
        }

        // Check Button 2 (Misrouted Luggage - Destination Berlin)
        if (gpio_get_level(PIN_BTN_ROGUE) == 0 && (now - last_press_time > debounce_ticks)) {
            last_press_time = now;
            printf("\n>>> [BUTTON] Triggered: Misrouted Baggage Push-Button (GPIO %d)\n", PIN_BTN_ROGUE);
            dispatch_tag_event("55-66-77-88", SCAN_STATUS_ROGUE);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/**
 * @brief Task 3: Dual RFID Scanner (Physical MFRC522 + Serial Terminal)
 */
void task_scanner_rfid(void *pvParameters)
{
    uint8_t str[16];
    char last_uid_str[32] = { 0 };
    TickType_t last_scan_time = 0;

    // Set STDIN to non-blocking mode for interactive keyboard triggers
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    uint8_t version = mfrc522_read_reg(VersionReg);
    printf("[RC522 STATUS] Chip Version on SPI2: 0x%02X (Valid if 0x91 or 0x92)\n", version);

    print_instructions_banner();

    while (1) {
        // 1. Listen for serial keyboard commands
        char input_ch = 0;
        int bytes_read = read(STDIN_FILENO, &input_ch, 1);
        if (bytes_read > 0) {
            if (input_ch == '1' || input_ch == 'g' || input_ch == 'G') {
                printf("\n>>> [SERIAL] Tag Simulation: 11-22-33-44 (Flight IB3166 - Valid)\n");
                dispatch_tag_event("11-22-33-44", SCAN_STATUS_VALID);
            } else if (input_ch == '2' || input_ch == 'y' || input_ch == 'Y') {
                printf("\n>>> [SERIAL] Tag Simulation: 55-66-77-88 (Destination Berlin - Rogue)\n");
                dispatch_tag_event("55-66-77-88", SCAN_STATUS_ROGUE);
            } else if (input_ch == '3' || input_ch == 'b' || input_ch == 'B') {
                printf("\n>>> [SERIAL] Tag Simulation: 01-02-03-04 (Flight IB3166 - Valid)\n");
                dispatch_tag_event("01-02-03-04", SCAN_STATUS_VALID);
            } else if (input_ch == 'h' || input_ch == 'H' || input_ch == '?') {
                print_instructions_banner();
            }
        }

        // 2. Poll physical MFRC522 RFID reader over SPI2
        if (mfrc522_request(str) == MI_OK) {
            if (mfrc522_anticoll(str) == MI_OK) {
                char current_uid[32];
                snprintf(current_uid, sizeof(current_uid), "%02X-%02X-%02X-%02X",
                         str[0], str[1], str[2], str[3]);

                TickType_t now = xTaskGetTickCount();
                if (strcmp(current_uid, last_uid_str) != 0 || (now - last_scan_time) > pdMS_TO_TICKS(2000)) {
                    strcpy(last_uid_str, current_uid);
                    last_scan_time = now;

                    printf("\n>>> [RFID ANTENNA] Physical PICC Tag Detected! UID: %s\n", current_uid);

                    scan_status_t status = SCAN_STATUS_ROGUE;
                    if ((str[0] == 0x11 && str[1] == 0x22 && str[2] == 0x33 && str[3] == 0x44) ||
                        (str[0] == 0xE2 && str[1] == 0x80 && str[2] == 0x68 && str[3] == 0x94) ||
                        (str[0] == 0x01 && str[1] == 0x02 && str[2] == 0x03 && str[3] == 0x04)) {
                        status = SCAN_STATUS_VALID;
                    }

                    dispatch_tag_event(current_uid, status);
                    mfrc522_halt();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/**
 * @brief Task 4: Cloud MQTT Publisher & Safety Interlock Manager
 * Receives luggage events from queue, drives status LEDs, and outputs MQTT payloads.
 */
void task_cloud_MQTT(void *pvParameters)
{
    luggage_event_t event;

    while (1) {
        if (xQueueReceive(xQueueLuggageEvent, &event, portMAX_DELAY) == pdTRUE) {
            
            // Safety Interlock: Cargo door closed
            if (event.is_door_closed) {
                printf("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
                printf("[SAFETY INTERLOCK] CARGO DOOR CLOSED (%d%%)\n", event.door_angle_pct);
                printf("Warning: Luggage intake prohibited while cargo door is locked.\n");
                printf("Action: Conveyor belt INHIBITED.\n");
                printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n");
                
                for (int i = 0; i < 4; i++) {
                    gpio_set_level(PIN_LED_ALERT_RED, 1);
                    vTaskDelay(pdMS_TO_TICKS(150));
                    gpio_set_level(PIN_LED_ALERT_RED, 0);
                    vTaskDelay(pdMS_TO_TICKS(150));
                }
                continue;
            }

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
                printf("  \"cargo_door_pct\": %d,\n", event.door_angle_pct);
                printf("  \"passenger_notification\": \"Luggage loaded into hold successfully\"\n");
                printf("}\n");
                printf("=======================================================\n\n");

                vTaskDelay(pdMS_TO_TICKS(800));
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
    printf("\nStarting AeroTrack IoT Firmware on ESP32-S2 DevKitM-1...\n");

    // 1. Configure Status Indicator LEDs (Outputs)
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << PIN_LED_VALID_GREEN) | (1ULL << PIN_LED_ALERT_RED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);

    // Boot LED Self-Test: Turn both LEDs ON for 1.5 seconds to verify polarity & wiring
    gpio_set_level(PIN_LED_VALID_GREEN, 1);
    gpio_set_level(PIN_LED_ALERT_RED, 1);
    vTaskDelay(pdMS_TO_TICKS(1500));
    gpio_set_level(PIN_LED_VALID_GREEN, 0);
    gpio_set_level(PIN_LED_ALERT_RED, 0);

    // 2. Configure Physical Push-Buttons (Inputs with Internal Pull-Ups)
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << PIN_BTN_VALID) | (1ULL << PIN_BTN_ROGUE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

    // 3. Configure Native SPI2 Bus on ESP32-S2 (GPIOs 33, 34, 35, 36, 37)
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_RC522_MISO,
        .mosi_io_num = PIN_RC522_MOSI,
        .sclk_io_num = PIN_RC522_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_RC522_SDA,
        .queue_size = 7,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi_rfid_handle));

    // 4. Initialize MFRC522 RFID Reader
    mfrc522_init();

    // 5. Configure ADC1 for Analog Potentiometers (GPIO 1 & GPIO 2)
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
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CH_DOOR_ANGLE, &config));

    // 6. Create FreeRTOS Event Queue
    xQueueLuggageEvent = xQueueCreate(10, sizeof(luggage_event_t));

    // 7. Launch FreeRTOS Tasks
    xTaskCreate(task_telemetry,    "task_telemetry",    4096, NULL, 3, NULL);
    xTaskCreate(task_buttons,      "task_buttons",      2048, NULL, 4, NULL);
    xTaskCreate(task_scanner_rfid, "task_scanner_rfid", 4096, NULL, 5, NULL);
    xTaskCreate(task_cloud_MQTT,   "task_cloud_MQTT",   4096, NULL, 4, NULL);

    printf("AeroTrack IoT System Initialized Successfully.\n");
}
