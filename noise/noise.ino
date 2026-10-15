/*
 * ESP32 injetor de ruido.
 * RX2 (GPIO 16) observa o barramento UART; D18 aciona a base do NPN.
 * Comandos no Monitor Serial: 0=desligado, 1=um bit, 2=dois bits, 3=rajada,
 * r=reinicia a contagem prevista de sequencia.
 */

#include <Arduino.h>
#include <esp_timer.h>
#include <driver/gpio.h>

static const int RX2_PIN = 16;
static const int TX2_PIN = 17;  // Nao e ligado nesta placa.
static const int NOISE_PIN = 18;
static const uint32_t BAUD_RATE = 115200;
static const uint8_t SOF = 0xA5;
static const uint32_t BIT_US = 9;  // 1/115200 = 8,68 us; inteiro usado pelo esp_timer.

struct Pulse {
  uint32_t delayUs;
  uint32_t durationUs;
};

Pulse pulses[2];
uint8_t timerIndex[2] = {0, 1};
esp_timer_handle_t startTimers[2];
esp_timer_handle_t endTimers[2];
uint8_t mode = 0;
uint16_t predictedSequence = 0;

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

void makeProtectedBytes(uint16_t sequence, uint8_t bytes[8]) {
  const uint16_t voltageMv = 3200 + (sequence % 300);
  const uint16_t temperatureCenti = 2400 + (sequence % 200);
  bytes[0] = highByte(sequence);
  bytes[1] = lowByte(sequence);
  bytes[2] = highByte(voltageMv);
  bytes[3] = lowByte(voltageMv);
  bytes[4] = highByte(temperatureCenti);
  bytes[5] = lowByte(temperatureCenti);
  const uint16_t crc = crc16Ccitt(bytes, 6);
  bytes[6] = highByte(crc);
  bytes[7] = lowByte(crc);
}

void endPulse(void *) {
  gpio_set_level(static_cast<gpio_num_t>(NOISE_PIN), 0);
}

void startPulse(void *argument) {
  const uint8_t i = *static_cast<uint8_t *>(argument);
  gpio_set_level(static_cast<gpio_num_t>(NOISE_PIN), 1);
  esp_timer_start_once(endTimers[i], pulses[i].durationUs);
}

// Agenda um pulso em um bit de dados do payload (bytes 2 a 5 do campo protegido).
// O byte ja esta longe do marcador, para dar tempo ao temporizador disparar.
uint32_t scheduleDataBit(uint8_t protectedByte, uint8_t dataBit) {
  // Apos o marcador, cada byte UART tem: inicio(0), 8 dados LSB-first, parada(1).
  const uint32_t bitFromNextStart = protectedByte * 10UL + 1UL + dataBit;
  return bitFromNextStart * BIT_US;
}

bool bitWouldChange(const uint8_t bytes[8], uint8_t protectedByte, uint8_t dataBit) {
  return (bytes[protectedByte] & (1U << dataBit)) != 0;
}

void armTimers(uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    esp_timer_stop(startTimers[i]);
    esp_timer_stop(endTimers[i]);
    esp_timer_start_once(startTimers[i], pulses[i].delayUs);
  }
}

void planInjection() {
  if (mode == 0) {
    ++predictedSequence;
    return;
  }

  uint8_t bytes[8];
  makeProtectedBytes(predictedSequence, bytes);
  const uint32_t timestamp = micros();

  if (mode == 1 || mode == 2) {
    const uint8_t firstByte = random(2, 6);
    const uint8_t firstBit = random(0, 8);
    pulses[0] = {scheduleDataBit(firstByte, firstBit), BIT_US};
    const bool firstChanged = bitWouldChange(bytes, firstByte, firstBit);

    if (mode == 1) {
      Serial.printf("NOISE,seq=%u,modo=1,byte=%u,bit=%u,t=%lu,mudou=%s\n",
                    predictedSequence, firstByte, firstBit, timestamp, firstChanged ? "SIM" : "NAO");
      armTimers(1);
    } else {
      uint8_t secondByte = random(2, 6);
      uint8_t secondBit = random(0, 8);
      while (secondByte == firstByte && secondBit == firstBit) {
        secondByte = random(2, 6);
        secondBit = random(0, 8);
      }
      pulses[1] = {scheduleDataBit(secondByte, secondBit), BIT_US};
      const bool secondChanged = bitWouldChange(bytes, secondByte, secondBit);
      Serial.printf("NOISE,seq=%u,modo=2,b1=%u.%u,b2=%u.%u,t=%lu,mudou=%s/%s\n",
                    predictedSequence, firstByte, firstBit, secondByte, secondBit, timestamp,
                    firstChanged ? "SIM" : "NAO", secondChanged ? "SIM" : "NAO");
      armTimers(2);
    }
  } else {  // modo 3: rajada dentro de um unico byte do payload.
    const uint8_t byteIndex = random(2, 6);
    const uint8_t length = random(3, 9);
    const uint8_t firstBit = random(0, 9 - length);
    uint8_t changedMask = 0;
    for (uint8_t bit = firstBit; bit < firstBit + length; ++bit) {
      if (bitWouldChange(bytes, byteIndex, bit)) changedMask |= (1U << bit);
    }
    pulses[0] = {scheduleDataBit(byteIndex, firstBit), static_cast<uint32_t>(length) * BIT_US};
    Serial.printf("NOISE,seq=%u,modo=3,byte=%u,bits=%u..%u,t=%lu,mascara_bits_que_mudam=0x%02X\n",
                  predictedSequence, byteIndex, firstBit, firstBit + length - 1, timestamp, changedMask);
    armTimers(1);
  }
  ++predictedSequence;
}

void readCommands() {
  while (Serial.available()) {
    const char command = static_cast<char>(Serial.read());
    if (command >= '0' && command <= '3') {
      mode = command - '0';
      Serial.printf("NOISE,modo=%u\n", mode);
    } else if (command == 'r' || command == 'R') {
      predictedSequence = 0;
      Serial.println("NOISE,sequencia prevista reiniciada");
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(BAUD_RATE, SERIAL_8N1, RX2_PIN, TX2_PIN);
  pinMode(NOISE_PIN, OUTPUT);
  digitalWrite(NOISE_PIN, LOW);
  randomSeed(esp_random());

  for (uint8_t i = 0; i < 2; ++i) {
    esp_timer_create_args_t startArgs = {};
    startArgs.callback = &startPulse;
    startArgs.arg = &timerIndex[i];
    startArgs.name = "noise_start";
    esp_timer_create(&startArgs, &startTimers[i]);

    esp_timer_create_args_t endArgs = {};
    endArgs.callback = &endPulse;
    endArgs.name = "noise_end";
    esp_timer_create(&endArgs, &endTimers[i]);
  }
  Serial.println("NOISE pronto. Digite 0, 1, 2 ou 3; use r antes de reiniciar o TX.");
}

void loop() {
  readCommands();
  while (Serial2.available()) {
    if (static_cast<uint8_t>(Serial2.read()) == SOF) {
      planInjection();
    }
  }
}
