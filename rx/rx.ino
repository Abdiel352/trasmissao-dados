/*
 * ESP32 receptor - Trabalho de telemetria UART
 * Liga RX2 (GPIO 16) ao barramento. As linhas CSV saem no USB (Serial).
 */

#include <Arduino.h>

static const int RX2_PIN = 16;  // Pino RX2 da placa.
static const int TX2_PIN = 17;  // Nao e ligado nesta placa.
static const uint32_t BAUD_RATE = 115200;
static const uint8_t SOF = 0xA5;
static const uint8_t DEBUG_SOF = 0x5A;
static const uint32_t IN_FRAME_TIMEOUT_MS = 30;

enum ParserState { WAIT_SOF, READ_FRAME, READ_DEBUG };
ParserState state = WAIT_SOF;
uint8_t frame[9];
uint8_t debugCopy[7];
uint8_t indexInBuffer = 0;
uint32_t frameStartedAt = 0;
uint16_t acceptedSequence = 0;
bool crcPassed = false;
uint32_t checkTimeUs = 0;
bool haveExpectedSequence = false;
uint16_t expectedSequence = 0;

uint32_t okCount = 0, corruptCount = 0, lostCount = 0, undetectedCount = 0;

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

void resetParser() {
  state = WAIT_SOF;
  indexInBuffer = 0;
}

void printSummary() {
  Serial.printf("RESUMO,OK=%lu,CORROMPIDO=%lu,PERDIDO=%lu,NAO_DETECTADO=%lu\n",
                okCount, corruptCount, lostCount, undetectedCount);
}

void accountSequence(uint16_t sequence) {
  if (haveExpectedSequence && sequence != expectedSequence) {
    const uint16_t gap = static_cast<uint16_t>(sequence - expectedSequence);
    // Delta abaixo de 32768 e tratado como lacuna; maior indica repeticao/fora de ordem.
    if (gap > 0 && gap < 32768) {
      lostCount += gap;
      Serial.printf("CSV,%u,,PERDIDO,,0,gap=%u\n", expectedSequence, gap);
    } else {
      Serial.printf("RX,AVISO,sequencia repetida ou fora de ordem: %u\n", sequence);
    }
  }
  expectedSequence = sequence + 1;  // uint16_t faz o wrap-around naturalmente.
  haveExpectedSequence = true;
}

void finishFrame() {
  const uint16_t debugSequence = word(debugCopy[1], debugCopy[2]);
  const bool debugPayloadMatches =
      debugCopy[0] == DEBUG_SOF && debugSequence == acceptedSequence &&
      memcmp(&frame[3], &debugCopy[3], 4) == 0;

  const uint16_t receivedCrc = word(frame[7], frame[8]);
  const uint16_t calculatedCrc = crc16Ccitt(&frame[1], 6);
  String status;

  if (!crcPassed) {
    status = "CORROMPIDO";
    ++corruptCount;
  } else if (!debugPayloadMatches) {
    status = "NAO_DETECTADO";
    ++undetectedCount;
    accountSequence(acceptedSequence);
  } else {
    status = "OK";
    ++okCount;
    accountSequence(acceptedSequence);
  }

  Serial.printf("CSV,%u,%04X/%04X,%s,%s,%lu\n",
                acceptedSequence, calculatedCrc, receivedCrc, status.c_str(),
                debugPayloadMatches ? "SIM" : "NAO", checkTimeUs);
  printSummary();
  resetParser();
}

void acceptFrameByte(uint8_t byteReceived) {
  if (state == WAIT_SOF) {
    if (byteReceived == SOF) {
      frame[0] = byteReceived;
      indexInBuffer = 1;
      frameStartedAt = millis();
      state = READ_FRAME;
    }
    return;
  }

  if (state == READ_FRAME) {
    frame[indexInBuffer++] = byteReceived;
    if (indexInBuffer == sizeof(frame)) {
      acceptedSequence = word(frame[1], frame[2]);
      const uint32_t startedCheckAt = micros();
      const uint16_t receivedCrc = word(frame[7], frame[8]);
      crcPassed = crc16Ccitt(&frame[1], 6) == receivedCrc;
      checkTimeUs = micros() - startedCheckAt;
      indexInBuffer = 0;
      state = READ_DEBUG;
    }
    return;
  }

  debugCopy[indexInBuffer++] = byteReceived;
  if (indexInBuffer == sizeof(debugCopy)) {
    finishFrame();
  }
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(BAUD_RATE, SERIAL_8N1, RX2_PIN, TX2_PIN);
  delay(500);
  Serial.println("sequencia,crc_calculado/recebido,status,payload_confere,tempo_verificacao_us");
}

void loop() {
  while (Serial2.available()) {
    acceptFrameByte(static_cast<uint8_t>(Serial2.read()));
  }

  if (state != WAIT_SOF && millis() - frameStartedAt > IN_FRAME_TIMEOUT_MS) {
    ++lostCount;
    Serial.println("CSV,, ,PERDIDO,NAO,0,timeout_dentro_do_quadro");
    printSummary();
    resetParser();
  }
}
