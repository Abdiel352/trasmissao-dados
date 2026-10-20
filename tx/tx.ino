/*
 * ESP32 TX - telemetria UART para o trabalho de deteccao de erros.
 * Pino fisico usado: TX2 (GPIO 17). RX2 e inicializado, mas nao e conectado.
 *
 * Monitor Serial (115200): c=CRC-16, f=Fletcher-16, 0..3=modo,
 * r=rodar 1000 quadros, ?=mostrar configuracao.
 */

#include <Arduino.h>

static const int RX2_PIN = 16;
static const int TX2_PIN = 17;
static const uint32_t BAUD_RATE = 115200;
static const uint8_t SOF = 0xA5;
static const uint8_t DEBUG_SOF = 0x5A;
static const uint8_t PROTOCOL_VERSION = 1;
static const uint8_t PAYLOAD_LENGTH = 4;
static const uint16_t FRAMES_PER_RUN = 1000;
static const uint32_t FRAME_INTERVAL_MS = 20;

enum Technique : uint8_t { TECH_CRC16 = 1, TECH_FLETCHER16 = 2 };
Technique selectedTechnique = TECH_CRC16;
uint8_t declaredNoiseMode = 0;
bool experimentRunning = false;

struct TimingStats {
  uint32_t minimumUs = UINT32_MAX;
  uint32_t maximumUs = 0;
  uint64_t totalUs = 0;
  uint32_t samples = 0;
};

void addTiming(TimingStats &stats, uint32_t valueUs) {
  if (valueUs < stats.minimumUs) stats.minimumUs = valueUs;
  if (valueUs > stats.maximumUs) stats.maximumUs = valueUs;
  stats.totalUs += valueUs;
  ++stats.samples;
}

const char *techniqueName(Technique technique) {
  return technique == TECH_CRC16 ? "CRC16_CCITT" : "FLETCHER16";
}

uint16_t crc16Ccitt(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  while (length--) {
    crc ^= static_cast<uint16_t>(*data++) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

uint16_t fletcher16Residue(const uint8_t *data, size_t length) {
  uint16_t sum1 = 0;
  uint16_t sum2 = 0;
  while (length--) {
    sum1 = (sum1 + *data++) % 255;
    sum2 = (sum2 + sum1) % 255;
  }
  return static_cast<uint16_t>((sum2 << 8) | sum1);
}

uint16_t fletcher16CheckBytes(const uint8_t *data, size_t length) {
  const uint16_t residue = fletcher16Residue(data, length);
  const uint16_t sum1 = residue & 0xFF;
  const uint16_t sum2 = residue >> 8;
  const uint8_t check1 = static_cast<uint8_t>((255 - ((sum1 + sum2) % 255)) % 255);
  const uint8_t check2 = static_cast<uint8_t>((255 - ((sum1 + check1) % 255)) % 255);
  return static_cast<uint16_t>((check1 << 8) | check2);
}

void makePayload(uint16_t sequence, uint8_t payload[PAYLOAD_LENGTH]) {
  const uint16_t voltageMv = 3200 + (sequence % 300);
  const uint16_t temperatureCenti = 2400 + (sequence % 200);
  payload[0] = highByte(voltageMv);
  payload[1] = lowByte(voltageMv);
  payload[2] = highByte(temperatureCenti);
  payload[3] = lowByte(temperatureCenti);
}

uint32_t buildAndSendFrame(uint16_t sequence) {
  uint8_t payload[PAYLOAD_LENGTH];
  makePayload(sequence, payload);

  // SOF | LENGTH | VERSION | TECH | MODE | SEQ(2) | PAYLOAD_LEN | PAYLOAD(4) | CODE
  uint8_t frame[14];
  const uint8_t bytesAfterLength = 1 + 1 + 1 + 2 + 1 + PAYLOAD_LENGTH + 2;
  frame[0] = SOF;
  frame[1] = bytesAfterLength;
  frame[2] = PROTOCOL_VERSION;
  frame[3] = selectedTechnique;
  frame[4] = declaredNoiseMode;
  frame[5] = highByte(sequence);
  frame[6] = lowByte(sequence);
  frame[7] = PAYLOAD_LENGTH;
  memcpy(&frame[8], payload, PAYLOAD_LENGTH);

  // Campos protegidos: LENGTH ate o ultimo byte do payload (11 bytes).
  const uint8_t protectedLength = 11;
  const uint32_t calculationStartedUs = micros();
  if (selectedTechnique == TECH_CRC16) {
    const uint16_t crc = crc16Ccitt(&frame[1], protectedLength);
    frame[12] = highByte(crc);
    frame[13] = lowByte(crc);
  } else {
    const uint16_t check = fletcher16CheckBytes(&frame[1], protectedLength);
    frame[12] = highByte(check);
    frame[13] = lowByte(check);
  }
  const uint32_t calculationTimeUs = micros() - calculationStartedUs;
  Serial2.write(frame, 2 + bytesAfterLength);

  // Copia verdadeira, enviada depois da decisao e fora da regiao protegida.
  const uint8_t debugCopy[12] = {
      DEBUG_SOF, 10, PROTOCOL_VERSION, static_cast<uint8_t>(selectedTechnique),
      declaredNoiseMode, highByte(sequence), lowByte(sequence), PAYLOAD_LENGTH,
      payload[0], payload[1], payload[2], payload[3]
  };
  Serial2.write(debugCopy, sizeof(debugCopy));
  Serial2.flush();
  return calculationTimeUs;
}

void printConfiguration() {
  const uint8_t mainFrameBytes = 2 + (1 + 1 + 1 + 2 + 1 + PAYLOAD_LENGTH + 2);
  Serial.printf("CONFIG,tecnica=%s,modo=%u,quadros=%u,intervalo_ms=%lu,bytes_quadro=%u,overhead=%u\n",
                techniqueName(selectedTechnique), declaredNoiseMode, FRAMES_PER_RUN,
                static_cast<unsigned long>(FRAME_INTERVAL_MS), mainFrameBytes,
                mainFrameBytes - PAYLOAD_LENGTH);
}

void runExperiment() {
  experimentRunning = true;
  TimingStats calculationStats;
  TimingStats intervalStats;
  uint32_t previousFrameStartedUs = 0;
  printConfiguration();
  Serial.println("TX_RUN,INICIO");

  for (uint16_t sequence = 0; sequence < FRAMES_PER_RUN; ++sequence) {
    const uint32_t frameStartedUs = micros();
    if (sequence > 0) addTiming(intervalStats, frameStartedUs - previousFrameStartedUs);
    previousFrameStartedUs = frameStartedUs;
    addTiming(calculationStats, buildAndSendFrame(sequence));
    delay(FRAME_INTERVAL_MS);
  }

  const double calculationAverage = calculationStats.samples
      ? static_cast<double>(calculationStats.totalUs) / calculationStats.samples : 0.0;
  const double intervalAverage = intervalStats.samples
      ? static_cast<double>(intervalStats.totalUs) / intervalStats.samples : 0.0;
  Serial.printf("TX_METRICAS,tecnica=%s,modo=%u,calculo_min_us=%lu,calculo_medio_us=%.2f,calculo_max_us=%lu,intervalo_min_us=%lu,intervalo_medio_us=%.2f,intervalo_max_us=%lu\n",
                techniqueName(selectedTechnique), declaredNoiseMode,
                static_cast<unsigned long>(calculationStats.minimumUs), calculationAverage,
                static_cast<unsigned long>(calculationStats.maximumUs),
                static_cast<unsigned long>(intervalStats.minimumUs), intervalAverage,
                static_cast<unsigned long>(intervalStats.maximumUs));
  Serial.println("TX_RUN,FIM");
  experimentRunning = false;
}

void selfTest() {
  uint8_t sample[11] = {12, PROTOCOL_VERSION, TECH_CRC16, 0, 0, 1,
                        PAYLOAD_LENGTH, 0x0C, 0x80, 0x09, 0x60};
  uint8_t crcMessage[13];
  memcpy(crcMessage, sample, sizeof(sample));
  const uint16_t crc = crc16Ccitt(sample, sizeof(sample));
  crcMessage[11] = highByte(crc);
  crcMessage[12] = lowByte(crc);
  const bool crcOk = crc16Ccitt(crcMessage, sizeof(crcMessage)) == 0;

  sample[0] = 12;
  uint8_t fletcherMessage[13];
  memcpy(fletcherMessage, sample, sizeof(sample));
  const uint16_t fletcherCheck = fletcher16CheckBytes(sample, sizeof(sample));
  fletcherMessage[11] = highByte(fletcherCheck);
  fletcherMessage[12] = lowByte(fletcherCheck);
  const bool fletcherOk = fletcher16Residue(fletcherMessage, sizeof(fletcherMessage)) == 0;
  Serial.printf("SELFTEST,CRC=%s,FLETCHER=%s\n",
                crcOk ? "OK" : "FALHOU", fletcherOk ? "OK" : "FALHOU");
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(BAUD_RATE, SERIAL_8N1, RX2_PIN, TX2_PIN);
  delay(500);
  selfTest();
  Serial.println("TX pronto: c=CRC, f=Fletcher, 0..3=modo, r=rodar, ?=status");
  printConfiguration();
}

void loop() {
  if (experimentRunning || !Serial.available()) return;
  const char command = static_cast<char>(Serial.read());
  if (command == 'c' || command == 'C') {
    selectedTechnique = TECH_CRC16;
    printConfiguration();
  } else if (command == 'f' || command == 'F' || command == 's' || command == 'S') {
    selectedTechnique = TECH_FLETCHER16;
    printConfiguration();
  } else if (command >= '0' && command <= '3') {
    declaredNoiseMode = static_cast<uint8_t>(command - '0');
    printConfiguration();
  } else if (command == 'r' || command == 'R') {
    runExperiment();
  } else if (command == '?') {
    printConfiguration();
  }
}
