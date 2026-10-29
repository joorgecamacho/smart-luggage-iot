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
// PINES DE HARDWARE (ESP32-S2 DevKitM-1)
// =======================================================
// Bus SPI2 nativo para MFRC522 (Pines libres de conflicto Flash)
#define PIN_RC522_RST           33   // Reset
#define PIN_RC522_SDA           34   // Chip Select (CS / SS)
#define PIN_RC522_MOSI          35   // MOSI
#define PIN_RC522_SCK           36   // Clock
#define PIN_RC522_MISO          37   // MISO

// LEDs Indicadores de Estado
#define PIN_LED_VALID_GREEN     16   // LED Verde: Maleta confirmada
#define PIN_LED_ALERT_RED       17   // LED Rojo: Alerta maleta desviada

// Canales ADC1 (Potenciometros)
#define ADC_CH_BELT_SPEED       ADC_CHANNEL_0  // GPIO 1: Velocidad Cinta
#define ADC_CH_DOOR_ANGLE       ADC_CHANNEL_1  // GPIO 2: Angulo Compuerta

// =======================================================
// REGISTROS MFRC522 & PROTOCOLO ISO 14443A
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

// Comandos PCD y PICC
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

// Manejadores globales
static spi_device_handle_t spi_rfid_handle;
static adc_oneshot_unit_handle_t adc1_handle;

// Estructura de eventos de equipaje
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

static QueueHandle_t xQueueLuggageEvent = NULL;
static int g_hold_loaded_count = 198;
static const int g_flight_total_expected = 200;

// =======================================================
// FUNCIONES SPI DE BAJO NIVEL (MFRC522)
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
    // 1. Reset fisico mediante pin RST (GPIO 33)
    gpio_set_direction(PIN_RC522_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_RC522_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RC522_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // 2. Soft reset por comando SPI
    mfrc522_write_reg(CommandReg, PCD_RESETPHASE);
    vTaskDelay(pdMS_TO_TICKS(50));

    // 3. Temporizador estandar ISO 14443A (25ms timeout para respuesta RF)
    mfrc522_write_reg(TModeReg, 0x80);      // TAuto=1: inicia automaticamente al finalizar transmision
    mfrc522_write_reg(TPrescalerReg, 0xA9); // 40 kHz (periodo de 25us)
    mfrc522_write_reg(TReloadRegH, 0x03);   // 0x03E8 = 1000 ticks = 25 ms
    mfrc522_write_reg(TReloadRegL, 0xE8);

    // 4. Modulacion ASK al 100% y presets CRC
    mfrc522_write_reg(TxASKReg, 0x40);
    mfrc522_write_reg(ModeReg, 0x3D);

    // 5. Encender antenas Tx1 y Tx2
    uint8_t tx_ctrl = mfrc522_read_reg(TxControlReg);
    if ((tx_ctrl & 0x03) != 0x03) {
        mfrc522_write_reg(TxControlReg, tx_ctrl | 0x03);
    }
}

static uint8_t mfrc522_to_card(uint8_t command, uint8_t *sendData, uint8_t sendLen,
                               uint8_t *backData, uint8_t *backLen, uint8_t validBits)
{
    uint8_t status = MI_ERR;
    uint8_t waitIRq = 0x30; // RxIRq y IdleIRq

    mfrc522_write_reg(CommandReg, PCD_IDLE);
    mfrc522_write_reg(ComIrqReg, 0x7F);      // Limpiar los 7 bits de interrupcion
    mfrc522_write_reg(FIFOLevelReg, 0x80);   // Flush buffer FIFO

    for (uint8_t i = 0; i < sendLen; i++) {
        mfrc522_write_reg(FIFODataReg, sendData[i]);
    }

    mfrc522_write_reg(BitFramingReg, validBits & 0x07);
    mfrc522_write_reg(CommandReg, command);

    if (command == PCD_TRANSCEIVE) {
        uint8_t cur = mfrc522_read_reg(BitFramingReg);
        mfrc522_write_reg(BitFramingReg, cur | 0x80); // StartSend = 1
    }

    // Esperar respuesta (25000 iteraciones para garantizar margen de tiempo sobre 25ms)
    uint32_t i = 25000;
    while (1) {
        uint8_t n = mfrc522_read_reg(ComIrqReg);
        uint8_t fifo_level = mfrc522_read_reg(FIFOLevelReg);

        // Si llego respuesta o hay bytes en el buffer FIFO, consideramos exito
        if ((n & waitIRq) || (fifo_level > 0)) {
            status = MI_OK;
            break;
        }
        if (n & 0x01) { // Timeout del temporizador sin tag presente
            status = MI_NOTAGERR;
            break;
        }
        if (--i == 0) {
            status = MI_ERR;
            break;
        }
    }

    // Limpiar bit StartSend
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
    // Intenta comando REQA primero (tarjetas idle) y WUPA (tarjetas despiertas o en hold)
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

static void print_instructions_banner(void)
{
    printf("\n=======================================================\n");
    printf("   AEROTRACK IoT - SISTEMA DE MONITOREO DE EQUIPAJE\n");
    printf("=======================================================\n");
    printf("Metodos de Escaneo Disponibles:\n");
    printf("  1. Lector MFRC522: Pulsa TAP o activa HOLD en la tarjeta\n");
    printf("  2. Teclado por Terminal (escribe y pulsa Enter):\n");
    printf("     [1] o [g] -> Maleta VALIDA   (Green Card: 11-22-33-44)\n");
    printf("     [2] o [y] -> Maleta DESVIADA (Yellow Card: 55-66-77-88)\n");
    printf("     [3] o [b] -> Maleta VALIDA   (Blue Card:  01-02-03-04)\n");
    printf("     [h]       -> Mostrar este menu de ayuda\n");
    printf("-------------------------------------------------------\n");
    printf("Potenciometros:\n");
    printf("  P1 (Arriba): Velocidad cinta transportadora (0-100%%)\n");
    printf("  P2 (Abajo):  Angulo compuerta bodega (>70%% = BLOQUEO)\n");
    printf("=======================================================\n\n");
}

// =======================================================
// TAREAS FREERTOS
// =======================================================

// Tarea 1: Telemetria de Sensores (Potenciometros ADC1)
void task_telemetry(void *pvParameters)
{
    int raw_speed = 0, raw_door = 0;

    while (1) {
        adc_oneshot_read(adc1_handle, ADC_CH_BELT_SPEED, &raw_speed);
        adc_oneshot_read(adc1_handle, ADC_CH_DOOR_ANGLE, &raw_door);

        int speed_pct = (raw_speed * 100) / 8191;
        int door_pct  = (raw_door * 100) / 8191;
        const char *door_state = (door_pct > 70) ? "CLOSED" : "OPEN";

        printf("[TELEMETRIA] Cinta: %d%% | Compuerta: %d%% (%s)\n", 
               speed_pct, door_pct, door_state);

        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}

// Despachar evento a la cola
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

// Tarea 2: Escaner RFID con Soporte Dual (Hardware MFRC522 + Terminal)
void task_scanner_rfid(void *pvParameters)
{
    uint8_t str[16];
    char last_uid_str[32] = { 0 };
    TickType_t last_scan_time = 0;

    // Configurar STDIN en modo no bloqueante para recibir pulsaciones
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    uint8_t version = mfrc522_read_reg(VersionReg);
    printf("[RC522 STATUS] Chip Version en SPI: 0x%02X (Correcto si es 0x91 o 0x92)\n", version);

    print_instructions_banner();

    while (1) {
        // 1. Escuchar comandos por terminal
        char input_ch = 0;
        int bytes_read = read(STDIN_FILENO, &input_ch, 1);
        if (bytes_read > 0) {
            if (input_ch == '1' || input_ch == 'g' || input_ch == 'G') {
                printf("\n>>> [SCANNER] Lectura de Tag RFID: 11-22-33-44 (Vuelo IB3166 - Verde)\n");
                dispatch_tag_event("11-22-33-44", SCAN_STATUS_VALID);
            } else if (input_ch == '2' || input_ch == 'y' || input_ch == 'Y') {
                printf("\n>>> [SCANNER] Lectura de Tag RFID: 55-66-77-88 (DESTINO BERLIN - Amarillo)\n");
                dispatch_tag_event("55-66-77-88", SCAN_STATUS_ROGUE);
            } else if (input_ch == '3' || input_ch == 'b' || input_ch == 'B') {
                printf("\n>>> [SCANNER] Lectura de Tag RFID: 01-02-03-04 (Vuelo IB3166 - Azul)\n");
                dispatch_tag_event("01-02-03-04", SCAN_STATUS_VALID);
            } else if (input_ch == 'h' || input_ch == 'H' || input_ch == '?') {
                print_instructions_banner();
            }
        }

        // 2. Polling del lector fisico MFRC522 por bus SPI
        if (mfrc522_request(str) == MI_OK) {
            if (mfrc522_anticoll(str) == MI_OK) {
                char current_uid[32];
                snprintf(current_uid, sizeof(current_uid), "%02X-%02X-%02X-%02X",
                         str[0], str[1], str[2], str[3]);

                TickType_t now = xTaskGetTickCount();
                if (strcmp(current_uid, last_uid_str) != 0 || (now - last_scan_time) > pdMS_TO_TICKS(2000)) {
                    strcpy(last_uid_str, current_uid);
                    last_scan_time = now;

                    printf("\n>>> [SCANNER] Deteccion fisica en Antena MFRC522! UID: %s\n", current_uid);

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

void task_feedback_led(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Tarea 4: Publicacion de Telemetria y Eventos MQTT
void task_cloud_MQTT(void *pvParameters)
{
    luggage_event_t event;

    while (1) {
        if (xQueueReceive(xQueueLuggageEvent, &event, portMAX_DELAY) == pdTRUE) {
            
            // Interlock de Seguridad: Compuerta Cerrada
            if (event.is_door_closed) {
                printf("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
                printf("[BLOQUEO DE SEGURIDAD] COMPUERTA CERRADA (%d%%)\n", event.door_angle_pct);
                printf("No se pueden ingresar maletas con la compuerta de bodega cerrada.\n");
                printf("Accion de seguridad: Cinta transportadora INHIBIDA.\n");
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
                
                // LED Verde ON durante confirmacion
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
                printf("  \"passenger_notification\": \"Maleta cargada en bodega con exito\"\n");
                printf("}\n");
                printf("=======================================================\n\n");

                vTaskDelay(pdMS_TO_TICKS(800));
                gpio_set_level(PIN_LED_VALID_GREEN, 0);

            } else {
                // Alerta de maleta desviada
                printf("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
                printf(">>> [MQTT ALERT] Topic: aerotrack/esp32/alerts\n");
                printf("{\n");
                printf("  \"flight_id\": \"IB3166\",\n");
                printf("  \"tag_uid\": \"%s\",\n", event.tag_uid);
                printf("  \"status\": \"ALERT_MISROUTED\",\n");
                printf("  \"warning\": \"MALETA NO PERTENECE A ESTE VUELO (Destino Berlin)\",\n");
                printf("  \"hold_action\": \"DETENER_CINTA\",\n");
                printf("  \"conveyor_speed_pct\": %d\n", event.conveyor_speed_pct);
                printf("}\n");
                printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n");

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
// INICIALIZACION DEL SISTEMA
// =======================================================
void app_main(void)
{
    printf("\nIniciando AeroTrack IoT en ESP32-S2 DevKitM-1...\n");

    // 1. Configurar LEDs
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_LED_VALID_GREEN) | (1ULL << PIN_LED_ALERT_RED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(PIN_LED_VALID_GREEN, 0);
    gpio_set_level(PIN_LED_ALERT_RED, 0);

    // 2. Bus SPI2 en ESP32-S2 (GPIOs nativos 33, 34, 35, 36, 37)
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

    // 3. Inicializar lector MFRC522
    mfrc522_init();

    // 4. Configurar ADC1 para Potenciometros (GPIO 1 y GPIO 2)
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

    // 5. Cola de eventos FreeRTOS
    xQueueLuggageEvent = xQueueCreate(5, sizeof(luggage_event_t));

    // 6. Iniciar Tareas FreeRTOS
    xTaskCreate(task_telemetry,    "task_telemetry",    4096, NULL, 3, NULL);
    xTaskCreate(task_scanner_rfid, "task_scanner_rfid", 4096, NULL, 5, NULL);
    xTaskCreate(task_feedback_led, "task_feedback_led", 2048, NULL, 2, NULL);
    xTaskCreate(task_cloud_MQTT,   "task_cloud_MQTT",   4096, NULL, 4, NULL);
}
