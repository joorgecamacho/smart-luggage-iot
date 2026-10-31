/*
 * AeroTrack IoT - Prototipo Fisico en Arduino ELEGOO UNO R3
 * Asignacion de Pines:
 *   - Boton 1 (Arriba): Pin 3 -> Maleta VALIDA (Vuelo IB3166)
 *   - Boton 2 (Abajo):  Pin 4 -> Maleta DESVIADA (Alerta Berlin)
 *   - Potenciometro:    Pin A0 -> Velocidad de cinta transportadora
 *   - LED Verde:        Pin 8 -> Confirmacion de carga
 *   - LED Rojo:         Pin 9 -> Alerta desvio
 */

// Pines de Hardware
const int PIN_BTN_VALID = 3;    // Boton de Arriba: Maleta Valida (Tag 11-22-33-44)
const int PIN_BTN_ROGUE = 4;    // Boton de Abajo:  Maleta Rogue  (Tag 55-66-77-88)
const int PIN_POT_SPEED = A0;   // Potenciometro (Velocidad Cinta)
const int PIN_LED_GREEN = 8;    // LED Verde
const int PIN_LED_RED   = 9;    // LED Rojo

// Estado del vuelo
int hold_loaded_count = 198;
const int flight_total_expected = 200;

// Temporizadores
unsigned long last_press_time = 0;
const unsigned long debounce_delay = 400; // 400ms antirrebote
unsigned long last_telemetry_time = 0;

void setup() {
  Serial.begin(115200);

  // Botones con resistencia interna de pull-up
  pinMode(PIN_BTN_VALID, INPUT_PULLUP);
  pinMode(PIN_BTN_ROGUE, INPUT_PULLUP);

  // LEDs
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);

  // Test de LEDs al arrancar: Los dejamos encendidos 1.5 segundos
  // para verificar de un vistazo que la polaridad y los pines son correctos
  digitalWrite(PIN_LED_GREEN, HIGH);
  digitalWrite(PIN_LED_RED, HIGH);
  delay(1500);
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_RED, LOW);

  Serial.println(F("\n======================================================="));
  Serial.println(F("  AEROTRACK IoT - PROTOCOLO INICIALIZADO (ELEGOO UNO)  "));
  Serial.println(F("======================================================="));
  Serial.println(F("Mapeo de Controles Fisicos:"));
  Serial.println(F("  - Boton ARRIBA (Pin 3): Maleta VALIDA (Vuelo IB3166)"));
  Serial.println(F("  - Boton ABAJO  (Pin 4): Maleta DESVIADA (Alerta Berlin)"));
  Serial.println(F("  - Potenciometro (A0):   Velocidad de cinta (0-100%)"));
  Serial.println(F("  - LED Verde    (Pin 8): Confirmacion de carga en bodega"));
  Serial.println(F("  - LED Rojo     (Pin 9): Alerta visual de seguridad"));
  Serial.println(F("=======================================================\n"));
}

void loop() {
  unsigned long now = millis();

  // 1. Leer potenciometro de velocidad (0% a 100%)
  int raw_pot = analogRead(PIN_POT_SPEED);
  int belt_speed_pct = map(raw_pot, 0, 1023, 0, 100);

  // Telemetria periodica cada 3 segundos
  if (now - last_telemetry_time >= 3000) {
    last_telemetry_time = now;
    Serial.print(F("[TELEMETRIA] Cinta transportadora: "));
    Serial.print(belt_speed_pct);
    Serial.print(F("% | Bodega: "));
    Serial.print(hold_loaded_count);
    Serial.print(F("/"));
    Serial.print(flight_total_expected);
    Serial.println(F(" maletas"));
  }

  // 2. Comprobar Boton de ARRIBA (Maleta Valida)
  if (digitalRead(PIN_BTN_VALID) == LOW && (now - last_press_time > debounce_delay)) {
    last_press_time = now;
    hold_loaded_count++;

    // Encender LED Verde
    digitalWrite(PIN_LED_GREEN, HIGH);

    Serial.println(F("\n======================================================="));
    Serial.println(F(">>> [MQTT PUB] Topic: aerotrack/esp32/luggage/scan"));
    Serial.println(F("{"));
    Serial.println(F("  \"flight_id\": \"IB3166\","));
    Serial.println(F("  \"tag_uid\": \"11-22-33-44\","));
    Serial.println(F("  \"status\": \"LOADED_CONFIRMED\","));
    Serial.print(F("  \"hold_tally\": \""));
    Serial.print(hold_loaded_count);
    Serial.print(F("/"));
    Serial.print(flight_total_expected);
    Serial.println(F("\","));
    Serial.print(F("  \"conveyor_speed_pct\": "));
    Serial.print(belt_speed_pct);
    Serial.println(F(","));
    Serial.println(F("  \"passenger_notification\": \"Maleta cargada en bodega con exito\""));
    Serial.println(F("}"));
    Serial.println(F("=======================================================\n"));

    delay(800);
    digitalWrite(PIN_LED_GREEN, LOW);
  }

  // 3. Comprobar Boton de ABAJO (Maleta Desviada / Rogue)
  if (digitalRead(PIN_BTN_ROGUE) == LOW && (now - last_press_time > debounce_delay)) {
    last_press_time = now;

    Serial.println(F("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
    Serial.println(F(">>> [MQTT ALERT] Topic: aerotrack/esp32/alerts"));
    Serial.println(F("{"));
    Serial.println(F("  \"flight_id\": \"IB3166\","));
    Serial.println(F("  \"tag_uid\": \"55-66-77-88\","));
    Serial.println(F("  \"status\": \"ALERT_MISROUTED\","));
    Serial.println(F("  \"warning\": \"MALETA NO PERTENECE A ESTE VUELO (Destino Berlin)\","));
    Serial.println(F("  \"hold_action\": \"DETENER_CINTA\","));
    Serial.print(F("  \"conveyor_speed_pct\": "));
    Serial.println(belt_speed_pct);
    Serial.println(F("}"));
    Serial.println(F("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"));

    // Alarma visual: Parpadeo rapido en LED Rojo
    for (int i = 0; i < 6; i++) {
      digitalWrite(PIN_LED_RED, HIGH);
      delay(150);
      digitalWrite(PIN_LED_RED, LOW);
      delay(150);
    }
  }

  delay(20);
}
