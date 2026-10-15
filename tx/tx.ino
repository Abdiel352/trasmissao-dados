/*
 * ESP32 transmissor - Trabalho de telemetria UART
 * Usa TX2 (GPIO 17) para enviar os quadros e Serial para o USB.
 */

#include <Arduino.h>

static const int RX2_PIN = 16;  // Nao e ligado nesta placa, mas e informado a UART2.
static const int TX2_PIN = 17;  // Pino escrito TX2 na placa.
static const uint32_t BAUD_RATE = 115200;
static const uint8_t SOF = 0xA5;
static const uint8_t DEBUG_SOF = 0x5A;
static const uint16_t FRAMES_PER_RUN = 1000;
static const uint32_t FRAME_INTERVAL_MS = 20;

uint16_t crc16Ccitt(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  while (length--) {
    crc ^= static_cast<uint16_t>(*data++) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
  }
  return crc;
}

void makePayload(uint16_t sequence, uint8_t payload[4]) {
  // Dados deterministas: permitem ao ESP32 de ruido saber se um pulso atingiu um bit 1.
  const uint16_t voltageMv = 3200 + (sequence % 300);
  const uint16_t temperatureCenti = 2400 + (sequence % 200);
  payload[0] = highByte(voltageMv);
  payload[1] = lowByte(voltageMv);
  payload[2] = highByte(temperatureCenti);
  payload[3] = lowByte(temperatureCenti);
}

void sendFrame(uint16_t sequence) {
  uint8_t payload[4];
  makePayload(sequence, payload);

  // Quadro protegido: SOF | sequencia(2) | payload(4) | CRC-16(2)
  uint8_t frame[9] = {
      SOF, highByte(sequence), lowByte(sequence),
      payload[0], payload[1], payload[2], payload[3], 0, 0
  };
  const uint16_t crc = crc16Ccitt(&frame[1], 6);
  frame[7] = highByte(crc);
  frame[8] = lowByte(crc);

  // Copia de referencia: fora da regiao protegida; o RX so le depois de decidir pelo CRC.
  const uint8_t debugCopy[7] = {
      DEBUG_SOF, highByte(sequence), lowByte(sequence),
      payload[0], payload[1], payload[2], payload[3]
  };

  Serial2.write(frame, sizeof(frame));
  Serial2.write(debugCopy, sizeof(debugCopy));
  Serial2.flush();

  Serial.printf("TX,seq=%u,payload=%02X%02X%02X%02X,crc=%04X\n",
                sequence, payload[0], payload[1], payload[2], payload[3], crc);
}

void runExperiment() {
  Serial.printf("TX,INICIO,%u quadros\n", FRAMES_PER_RUN);
  for (uint16_t sequence = 0; sequence < FRAMES_PER_RUN; ++sequence) {
    sendFrame(sequence);
    delay(FRAME_INTERVAL_MS);
  }
  Serial.println("TX,FIM");
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(BAUD_RATE, SERIAL_8N1, RX2_PIN, TX2_PIN);
  delay(500);
  Serial.println("TX pronto. Digite r no Monitor Serial para enviar 1000 quadros.");
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') {
    runExperiment();
  }
}
