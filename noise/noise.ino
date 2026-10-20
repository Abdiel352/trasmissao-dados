/*
 * ESP32 de ruido.
 * RX2 (GPIO 16) observa o barramento; D18 (GPIO 18) aciona o NPN.
 *
 * Monitor Serial (115200): 0=desligado, 1=um bit, 2=dois bits,
 * 3=rajada de 3..8 bits, k=padrao que quebra o Fletcher-16,
 * r=zerar sequencia, +=adiantar pulso, -=atrasar pulso, ?=status.
 */

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_timer.h>

static const int RX2_PIN = 16;
static const int TX2_PIN = 17;
static const int NOISE_PIN = 18;
static const uint32_t BAUD_RATE = 115200;
static const uint8_t SOF = 0xA5;
static const uint8_t DEBUG_SOF = 0x5A;
static const uint8_t CRC_BODY_LENGTH = 12;
static const uint8_t DEBUG_BODY_LENGTH = 10;
static const uint8_t PAYLOAD_LENGTH = 4;

enum NoiseParserState : uint8_t {
  FIND_MAIN,
  READ_MAIN_LENGTH,
  SKIP_MAIN_BODY,
  FIND_DEBUG,
  READ_DEBUG_LENGTH,
  SKIP_DEBUG_BODY
};

struct PulseEvent {
  volatile bool active = false;
  volatile bool finished = false;
  volatile int64_t actualStartUs = 0;
  volatile bool lineWasHighAtStart = false;
  uint32_t delayUs = 0;
  uint32_t durationUs = 0;
  uint16_t sequence = 0;
  uint8_t selectedMode = 0;
  uint8_t frameByte = 0;
  uint8_t firstBit = 0;
  uint8_t bitCount = 0;
  uint8_t changedMask = 0;
};

PulseEvent events[2];
uint8_t eventIndexes[2] = {0, 1};
esp_timer_handle_t startTimers[2];
esp_timer_handle_t endTimers[2];
NoiseParserState parserState = FIND_MAIN;
uint8_t bytesRemaining = 0;
uint8_t noiseMode = 0;  // 0..3 normais; 4 = demonstracao especial.
uint16_t predictedSequence = 0;
uint32_t timingAdvanceUs = 8;

const char *noiseModeName(uint8_t mode) {
  static const char *names[] = {"0", "1", "2", "3", "QUEBRA_FLETCHER"};
  return mode <= 4 ? names[mode] : "INVALIDO";
}

uint32_t bitPeriodsToUs(uint32_t periods) {
  return static_cast<uint32_t>((periods * 1000000ULL + BAUD_RATE / 2) / BAUD_RATE);
}

uint32_t delayToDataBit(uint8_t frameByteAfterSof, uint8_t dataBit) {
  // Cada byte UART: start + 8 bits LSB-first + stop = 10 periodos.
  uint32_t delayUs = bitPeriodsToUs(frameByteAfterSof * 10UL + 1UL + dataBit);
  return delayUs > timingAdvanceUs ? delayUs - timingAdvanceUs : 1;
}

void makePayload(uint16_t sequence, uint8_t payload[PAYLOAD_LENGTH]) {
  const uint16_t voltageMv = 3200 + (sequence % 300);
  const uint16_t temperatureCenti = 2400 + (sequence % 200);
  payload[0] = highByte(voltageMv);
  payload[1] = lowByte(voltageMv);
  payload[2] = highByte(temperatureCenti);
  payload[3] = lowByte(temperatureCenti);
}

void endPulse(void *argument) {
  const uint8_t index = *static_cast<uint8_t *>(argument);
  gpio_set_level(static_cast<gpio_num_t>(NOISE_PIN), 0);
  events[index].finished = true;
}

void startPulse(void *argument) {
  const uint8_t index = *static_cast<uint8_t *>(argument);
  events[index].actualStartUs = esp_timer_get_time();
  events[index].lineWasHighAtStart =
      gpio_get_level(static_cast<gpio_num_t>(RX2_PIN)) != 0;
  gpio_set_level(static_cast<gpio_num_t>(NOISE_PIN), 1);
  esp_timer_start_once(endTimers[index], events[index].durationUs);
}

void armEvent(uint8_t index, uint16_t sequence, uint8_t selectedMode,
              uint8_t frameByte, uint8_t firstBit, uint8_t bitCount,
              uint8_t changedMask) {
  esp_timer_stop(startTimers[index]);
  esp_timer_stop(endTimers[index]);
  PulseEvent &event = events[index];
  event.active = true;
  event.finished = false;
  event.actualStartUs = 0;
  event.lineWasHighAtStart = false;
  event.delayUs = delayToDataBit(frameByte, firstBit);
  event.durationUs = bitPeriodsToUs(bitCount);
  event.sequence = sequence;
  event.selectedMode = selectedMode;
  event.frameByte = frameByte;
  event.firstBit = firstBit;
  event.bitCount = bitCount;
  event.changedMask = changedMask;
  esp_timer_start_once(startTimers[index], event.delayUs);
}

void planInjection() {
  uint8_t payload[PAYLOAD_LENGTH];
  makePayload(predictedSequence, payload);

  if (noiseMode == 1 || noiseMode == 2) {
    const uint8_t payloadIndex1 = random(0, PAYLOAD_LENGTH);
    const uint8_t bit1 = random(0, 8);
    const uint8_t frameByte1 = 7 + payloadIndex1;
    const uint8_t mask1 = (payload[payloadIndex1] & (1U << bit1)) ? (1U << bit1) : 0;
    armEvent(0, predictedSequence, noiseMode, frameByte1, bit1, 1, mask1);

    if (noiseMode == 2) {
      uint8_t payloadIndex2;
      uint8_t bit2;
      uint8_t frameByte2;
      uint32_t firstPosition = frameByte1 * 10UL + bit1;
      do {
        payloadIndex2 = random(0, PAYLOAD_LENGTH);
        bit2 = random(0, 8);
        frameByte2 = 7 + payloadIndex2;
      } while (abs(static_cast<int32_t>(frameByte2 * 10UL + bit2) -
                        static_cast<int32_t>(firstPosition)) < 2);
      const uint8_t mask2 = (payload[payloadIndex2] & (1U << bit2)) ? (1U << bit2) : 0;
      armEvent(1, predictedSequence, noiseMode, frameByte2, bit2, 1, mask2);
    }
  } else if (noiseMode == 3) {
    const uint8_t payloadIndex = random(0, PAYLOAD_LENGTH);
    const uint8_t bitCount = random(3, 9);
    const uint8_t firstBit = random(0, 9 - bitCount);
    uint8_t changedMask = 0;
    for (uint8_t bit = firstBit; bit < firstBit + bitCount; ++bit) {
      if (payload[payloadIndex] & (1U << bit)) changedMask |= (1U << bit);
    }
    armEvent(0, predictedSequence, noiseMode, 7 + payloadIndex,
             firstBit, bitCount, changedMask);
  } else if (noiseMode == 4) {
    // No Fletcher-16, 0xFF e 0x00 sao equivalentes modulo 255.
    // Uma rajada de 8 bits que transforma FF em 00 passa sem deteccao.
    if (payload[1] == 0xFF) {
      armEvent(0, predictedSequence, noiseMode, 8, 0, 8, 0xFF);
    } else if (payload[3] == 0xFF) {
      armEvent(0, predictedSequence, noiseMode, 10, 0, 8, 0xFF);
    }
  }
  predictedSequence = static_cast<uint16_t>(predictedSequence + 1);
}

void reportFinishedEvents() {
  for (uint8_t index = 0; index < 2; ++index) {
    PulseEvent &event = events[index];
    if (!event.active || !event.finished) continue;
    noInterrupts();
    const int64_t actualStartUs = event.actualStartUs;
    const bool lineWasHighAtStart = event.lineWasHighAtStart;
    event.active = false;
    event.finished = false;
    interrupts();
    Serial.printf("INJECAO,seq=%u,modo=%s,byte_quadro=%u,bits=%u..%u,timestamp_us=%lld,duracao_us=%lu,linha_alta_antes=%s,mudou_no_inicio=%s,mascara_prevista=0x%02X\n",
                  event.sequence, noiseModeName(event.selectedMode),
                  event.frameByte, event.firstBit,
                  event.firstBit + event.bitCount - 1,
                  static_cast<long long>(actualStartUs),
                  static_cast<unsigned long>(event.durationUs),
                  lineWasHighAtStart ? "SIM" : "NAO",
                  lineWasHighAtStart ? "SIM" : "NAO", event.changedMask);
  }
}

void beginMainFrame() {
  planInjection();
  parserState = READ_MAIN_LENGTH;
}

void processObservedByte(uint8_t value) {
  switch (parserState) {
    case FIND_MAIN:
      if (value == SOF) beginMainFrame();
      break;

    case READ_MAIN_LENGTH:
      if (value == CRC_BODY_LENGTH) {
        bytesRemaining = value;
        parserState = SKIP_MAIN_BODY;
      } else {
        parserState = FIND_MAIN;
      }
      break;

    case SKIP_MAIN_BODY:
      if (--bytesRemaining == 0) parserState = FIND_DEBUG;
      break;

    case FIND_DEBUG:
      if (value == DEBUG_SOF) parserState = READ_DEBUG_LENGTH;
      else if (value == SOF) beginMainFrame();
      break;

    case READ_DEBUG_LENGTH:
      if (value == DEBUG_BODY_LENGTH) {
        bytesRemaining = value;
        parserState = SKIP_DEBUG_BODY;
      } else {
        parserState = FIND_MAIN;
      }
      break;

    case SKIP_DEBUG_BODY:
      if (--bytesRemaining == 0) parserState = FIND_MAIN;
      break;
  }
}

void printStatus() {
  Serial.printf("NOISE_STATUS,modo=%s,proxima_seq=%u,adiantamento_us=%lu\n",
                noiseModeName(noiseMode),
                predictedSequence, static_cast<unsigned long>(timingAdvanceUs));
}

void readCommands() {
  while (Serial.available()) {
    const char command = static_cast<char>(Serial.read());
    if (command >= '0' && command <= '3') {
      noiseMode = command - '0';
      printStatus();
    } else if (command == 'k' || command == 'K') {
      noiseMode = 4;
      printStatus();
    } else if (command == 'r' || command == 'R') {
      predictedSequence = 0;
      parserState = FIND_MAIN;
      Serial.println("NOISE,sequencia_e_parser_reiniciados");
    } else if (command == '+') {
      ++timingAdvanceUs;
      printStatus();
    } else if (command == '-') {
      if (timingAdvanceUs > 0) --timingAdvanceUs;
      printStatus();
    } else if (command == '?') {
      printStatus();
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(BAUD_RATE, SERIAL_8N1, RX2_PIN, TX2_PIN);
  pinMode(NOISE_PIN, OUTPUT);
  digitalWrite(NOISE_PIN, LOW);
  randomSeed(esp_random());

  for (uint8_t index = 0; index < 2; ++index) {
    esp_timer_create_args_t startArgs = {};
    startArgs.callback = &startPulse;
    startArgs.arg = &eventIndexes[index];
    startArgs.name = "noise_start";
    esp_timer_create(&startArgs, &startTimers[index]);

    esp_timer_create_args_t endArgs = {};
    endArgs.callback = &endPulse;
    endArgs.arg = &eventIndexes[index];
    endArgs.name = "noise_end";
    esp_timer_create(&endArgs, &endTimers[index]);
  }
  delay(500);
  Serial.println("NOISE pronto: 0..3=modo, k=quebrar Fletcher, r=reiniciar, +/-=calibrar");
  printStatus();
}

void loop() {
  readCommands();
  while (Serial2.available()) processObservedByte(static_cast<uint8_t>(Serial2.read()));
  reportFinishedEvents();
}
