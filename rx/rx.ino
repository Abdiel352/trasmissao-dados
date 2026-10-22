/*
 * ESP32 RX - receptor, classificador e gerador de CSV.
 * Pino fisico usado: RX2 (GPIO 16). TX2 nao e conectado.
 *
 * Monitor Serial (115200):
 *   r = preparar uma nova rodada (mantem tabelas acumuladas)
 *   t = imprimir as tabelas
 *   x = apagar todas as tabelas e preparar nova rodada
 *   p<sequencia> = mostrar os 14 bytes recebidos naquela sequencia
 */

#include <Arduino.h>

static const int RX2_PIN = 16;
static const int TX2_PIN = 17;
static const uint32_t BAUD_RATE = 115200;
static const uint8_t SOF = 0xA5;
static const uint8_t DEBUG_SOF = 0x5A;
static const uint8_t PROTOCOL_VERSION = 1;
static const uint8_t PAYLOAD_LENGTH = 4;
static const uint8_t CRC_BODY_LENGTH = 12;
static const uint8_t DEBUG_BODY_LENGTH = 10;
static const uint8_t MAX_BODY_LENGTH = 16;
static const uint8_t MAIN_FRAME_BYTES = 14;
static const uint16_t FRAMES_PER_RUN = 1000;
static const uint32_t INSIDE_FRAME_TIMEOUT_US = 3000;
static const uint32_t INTERFRAME_TIMEOUT_US = 50000;

enum Technique : uint8_t { TECH_CRC16 = 1, TECH_FLETCHER16 = 2 };
enum ParserState : uint8_t {
  SEARCH_MARKER,
  READ_MAIN_LENGTH,
  READ_MAIN_BODY,
  READ_DEBUG_LENGTH,
  READ_DEBUG_BODY
};
enum ResultStatus : uint8_t { STATUS_OK, STATUS_CORRUPT, STATUS_LOST, STATUS_UNDETECTED };

struct TimingStats {
  uint32_t minimumUs = UINT32_MAX;
  uint32_t maximumUs = 0;
  uint64_t totalUs = 0;
  uint32_t samples = 0;
};

struct ResultStats {
  uint32_t ok = 0;
  uint32_t corrupt = 0;
  uint32_t lost = 0;
  uint32_t undetected = 0;
  TimingStats verification;
  TimingStats recovery;
};

struct PendingMain {
  bool present = false;
  bool rawComplete = false;
  bool concludedCheck = false;
  bool checkPassed = false;
  uint8_t technique = 0;
  uint8_t mode = 0;
  uint16_t receivedSequence = 0;
  uint8_t payload[PAYLOAD_LENGTH] = {0};
  uint8_t rawFrame[MAIN_FRAME_BYTES] = {0};
  uint16_t residue = 0;
  uint32_t verificationUs = 0;
};

ParserState parserState = SEARCH_MARKER;
uint8_t mainBytes[1 + MAX_BODY_LENGTH];
uint8_t debugBytes[1 + MAX_BODY_LENGTH];
uint8_t expectedBodyLength = 0;
uint8_t bodyIndex = 0;
uint32_t lastParserByteUs = 0;
PendingMain pendingMain;
uint8_t receivedFrames[FRAMES_PER_RUN][MAIN_FRAME_BYTES];
bool receivedFrameAvailable[FRAMES_PER_RUN];
bool readingFrameSequence = false;
bool haveFrameSequenceDigit = false;
bool invalidFrameSequence = false;
uint16_t requestedFrameSequence = 0;
ResultStats tableStats[2][4];
TimingStats rxIntervalStats;
uint32_t previousMainMarkerUs = 0;
bool havePreviousMainMarker = false;
bool haveExpectedSequence = false;
uint16_t expectedSequence = 0;
bool waitingForRecovery = false;
uint32_t lossDetectedUs = 0;
bool runActive = false;
uint32_t lastReferenceFrameUs = 0;
uint8_t lastReferenceTechnique = TECH_CRC16;
uint8_t lastReferenceMode = 0;

void addTiming(TimingStats &stats, uint32_t valueUs) {
  if (valueUs < stats.minimumUs) stats.minimumUs = valueUs;
  if (valueUs > stats.maximumUs) stats.maximumUs = valueUs;
  stats.totalUs += valueUs;
  ++stats.samples;
}

const char *techniqueName(uint8_t technique) {
  if (technique == TECH_CRC16) return "CRC16_CCITT";
  if (technique == TECH_FLETCHER16) return "FLETCHER16";
  return "DESCONHECIDA";
}

const char *statusName(ResultStatus status) {
  switch (status) {
    case STATUS_OK: return "OK";
    case STATUS_CORRUPT: return "CORROMPIDO";
    case STATUS_LOST: return "PERDIDO";
    default: return "NAO_DETECTADO";
  }
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

void resetParser() {
  parserState = SEARCH_MARKER;
  expectedBodyLength = 0;
  bodyIndex = 0;
}

void markMalformedMain() {
  pendingMain = PendingMain();
  pendingMain.present = true;
  pendingMain.concludedCheck = false;
}

void evaluateMain() {
  pendingMain = PendingMain();
  pendingMain.present = true;
  pendingMain.rawComplete = true;
  pendingMain.rawFrame[0] = SOF;
  memcpy(&pendingMain.rawFrame[1], mainBytes, 1 + CRC_BODY_LENGTH);

  const uint8_t length = mainBytes[0];
  const uint8_t version = mainBytes[1];
  const uint8_t technique = mainBytes[2];
  const uint8_t mode = mainBytes[3];
  const uint8_t payloadLength = mainBytes[6];
  const bool lengthMatchesTechnique =
      (technique == TECH_CRC16 || technique == TECH_FLETCHER16) &&
      length == CRC_BODY_LENGTH;
  const bool structureValid = version == PROTOCOL_VERSION && mode <= 3 &&
      payloadLength == PAYLOAD_LENGTH && lengthMatchesTechnique;

  if (!structureValid) {
    pendingMain.concludedCheck = false;
    return;
  }

  pendingMain.technique = technique;
  pendingMain.mode = mode;
  pendingMain.receivedSequence = word(mainBytes[4], mainBytes[5]);
  memcpy(pendingMain.payload, &mainBytes[7], PAYLOAD_LENGTH);

  const uint32_t startedUs = micros();
  if (technique == TECH_CRC16) {
    pendingMain.residue = crc16Ccitt(mainBytes, 1 + length);
    pendingMain.checkPassed = pendingMain.residue == 0;
  } else {
    pendingMain.residue = fletcher16Residue(mainBytes, 1 + length);
    pendingMain.checkPassed = pendingMain.residue == 0;
  }
  pendingMain.verificationUs = micros() - startedUs;
  pendingMain.concludedCheck = true;
}

void printOneTable(uint8_t technique, uint8_t mode) {
  ResultStats &stats = tableStats[technique - 1][mode];
  const uint32_t total = stats.ok + stats.corrupt + stats.lost + stats.undetected;
  const uint32_t errorFrames = stats.corrupt + stats.undetected;
  const double nonDetectionRate = errorFrames
      ? 100.0 * static_cast<double>(stats.undetected) / errorFrames : 0.0;
  const double verificationAverage = stats.verification.samples
      ? static_cast<double>(stats.verification.totalUs) / stats.verification.samples : 0.0;
  const double recoveryAverage = stats.recovery.samples
      ? static_cast<double>(stats.recovery.totalUs) / stats.recovery.samples : 0.0;

  Serial.printf("TABELA,%s,modo=%u,OK=%lu,CORROMPIDO=%lu,PERDIDO=%lu,NAO_DETECTADO=%lu,TOTAL=%lu,TAXA_NAO_DETECCAO=%.4f%%,VERIFICACAO_MEDIA_US=%.2f,RECUPERACAO_MEDIA_MS=%.3f\n",
                techniqueName(technique), mode,
                static_cast<unsigned long>(stats.ok),
                static_cast<unsigned long>(stats.corrupt),
                static_cast<unsigned long>(stats.lost),
                static_cast<unsigned long>(stats.undetected),
                static_cast<unsigned long>(total), nonDetectionRate,
                verificationAverage, recoveryAverage / 1000.0);
}

void printAllTables() {
  for (uint8_t technique = TECH_CRC16; technique <= TECH_FLETCHER16; ++technique) {
    for (uint8_t mode = 0; mode <= 3; ++mode) printOneTable(technique, mode);
  }
  if (rxIntervalStats.samples) {
    const double average = static_cast<double>(rxIntervalStats.totalUs) / rxIntervalStats.samples;
    Serial.printf("RX_INTERVALO,min_us=%lu,medio_us=%.2f,max_us=%lu,timeout_interno_us=%lu,timeout_entre_quadros_us=%lu\n",
                  static_cast<unsigned long>(rxIntervalStats.minimumUs), average,
                  static_cast<unsigned long>(rxIntervalStats.maximumUs),
                  static_cast<unsigned long>(INSIDE_FRAME_TIMEOUT_US),
                  static_cast<unsigned long>(INTERFRAME_TIMEOUT_US));
  }
}

void printReceivedFrame(uint16_t sequence) {
  if (!receivedFrameAvailable[sequence]) {
    Serial.printf("QUADRO_RECEBIDO,seq=%u,indisponivel=quadro_principal_nao_montado\n", sequence);
    return;
  }
  const uint8_t *frame = receivedFrames[sequence];
  const uint16_t headerSequence = word(frame[5], frame[6]);
  Serial.printf("QUADRO_RECEBIDO,seq_referencia=%u,seq_cabecalho=%u,hex=",
                sequence, headerSequence);
  for (uint8_t index = 0; index < MAIN_FRAME_BYTES; ++index) {
    Serial.printf("%02X%s", frame[index], index + 1 == MAIN_FRAME_BYTES ? "\n" : " ");
  }
}

void recordResult(uint8_t technique, uint8_t mode, uint16_t sequence,
                  ResultStatus status, bool payloadMatches, uint16_t residue,
                  bool hasVerification, uint32_t verificationUs) {
  if (technique < TECH_CRC16 || technique > TECH_FLETCHER16 || mode > 3) return;
  ResultStats &stats = tableStats[technique - 1][mode];
  uint32_t recoveryUs = 0;

  if (status == STATUS_OK) ++stats.ok;
  else if (status == STATUS_CORRUPT) ++stats.corrupt;
  else if (status == STATUS_LOST) {
    ++stats.lost;
    if (!waitingForRecovery) {
      waitingForRecovery = true;
      lossDetectedUs = micros();
    }
  } else ++stats.undetected;

  if (hasVerification) addTiming(stats.verification, verificationUs);
  if (status != STATUS_LOST && waitingForRecovery) {
    recoveryUs = micros() - lossDetectedUs;
    addTiming(stats.recovery, recoveryUs);
    waitingForRecovery = false;
  }

  if (status == STATUS_LOST) {
    Serial.printf("CSV,%s,%u,%u,,PERDIDO,,0,0\n", techniqueName(technique), mode, sequence);
  } else {
    char residueText[5];
    snprintf(residueText, sizeof(residueText), "%04X", residue);
    Serial.printf("CSV,%s,%u,%u,%s,%s,%s,%lu,%lu\n",
                  techniqueName(technique), mode, sequence, residueText,
                  statusName(status), payloadMatches ? "SIM" : "NAO",
                  static_cast<unsigned long>(verificationUs),
                  static_cast<unsigned long>(recoveryUs));
  }
}

bool accountDebugSequence(uint8_t technique, uint8_t mode, uint16_t sequence) {
  if (!haveExpectedSequence) {
    // Cada rodada do transmissor sempre comeca em 0. Se a primeira copia de
    // depuracao recebida ja traz uma sequencia maior, os quadros iniciais
    // foram perdidos antes de o RX conseguir se sincronizar e tambem devem
    // entrar na tabela.
    for (uint16_t missingSequence = 0; missingSequence < sequence; ++missingSequence) {
      recordResult(technique, mode, missingSequence,
                   STATUS_LOST, false, 0, false, 0);
    }
    expectedSequence = static_cast<uint16_t>(sequence + 1);
    haveExpectedSequence = true;
    return true;
  }
  if (sequence == expectedSequence) {
    expectedSequence = static_cast<uint16_t>(sequence + 1);
    return true;
  }

  const uint16_t difference = static_cast<uint16_t>(sequence - expectedSequence);
  if (difference > 0 && difference < 0x8000) {
    for (uint16_t i = 0; i < difference; ++i) {
      recordResult(technique, mode, static_cast<uint16_t>(expectedSequence + i),
                   STATUS_LOST, false, 0, false, 0);
    }
  } else {
    Serial.printf("AVISO,sequencia_repetida_ou_fora_de_ordem,recebida=%u,esperada=%u\n",
                  sequence, expectedSequence);
    return false;
  }
  expectedSequence = static_cast<uint16_t>(sequence + 1);
  return true;
}

void evaluateDebug() {
  const uint8_t length = debugBytes[0];
  const uint8_t version = debugBytes[1];
  const uint8_t technique = debugBytes[2];
  const uint8_t mode = debugBytes[3];
  const uint16_t trueSequence = word(debugBytes[4], debugBytes[5]);
  const uint8_t payloadLength = debugBytes[6];
  if (length != DEBUG_BODY_LENGTH || version != PROTOCOL_VERSION ||
      technique < TECH_CRC16 || technique > TECH_FLETCHER16 || mode > 3 ||
      payloadLength != PAYLOAD_LENGTH) {
    Serial.println("AVISO,copia_de_depuracao_invalida");
    pendingMain = PendingMain();
    return;
  }

  // Depois do primeiro quadro valido, tecnica e modo fazem parte da
  // configuracao da rodada. A copia de depuracao nao e protegida: se esses
  // campos forem alterados pelo ruido, ela nao pode criar resultados em outra
  // tabela nem trocar a referencia usada pelos timeouts.
  if (runActive && (technique != lastReferenceTechnique || mode != lastReferenceMode)) {
    Serial.printf("AVISO,copia_de_depuracao_com_configuracao_invalida,tecnica=%u,modo=%u,esperado_tecnica=%u,esperado_modo=%u\n",
                  technique, mode, lastReferenceTechnique, lastReferenceMode);
    pendingMain = PendingMain();
    return;
  }

  if (!accountDebugSequence(technique, mode, trueSequence)) {
    pendingMain = PendingMain();
    return;
  }
  if (trueSequence < FRAMES_PER_RUN && pendingMain.rawComplete) {
    memcpy(receivedFrames[trueSequence], pendingMain.rawFrame, MAIN_FRAME_BYTES);
    receivedFrameAvailable[trueSequence] = true;
  }
  runActive = true;
  lastReferenceFrameUs = micros();
  lastReferenceTechnique = technique;
  lastReferenceMode = mode;
  ResultStatus status;
  bool payloadMatches = false;
  bool hasVerification = false;
  uint16_t residue = 0;
  uint32_t verificationUs = 0;

  if (!pendingMain.present || !pendingMain.concludedCheck) {
    status = STATUS_LOST;
  } else {
    payloadMatches = memcmp(pendingMain.payload, &debugBytes[7], PAYLOAD_LENGTH) == 0;
    hasVerification = true;
    residue = pendingMain.residue;
    verificationUs = pendingMain.verificationUs;
    if (!pendingMain.checkPassed) status = STATUS_CORRUPT;
    else if (!payloadMatches) status = STATUS_UNDETECTED;
    else status = STATUS_OK;

    if (pendingMain.receivedSequence != trueSequence) {
      Serial.printf("AVISO,sequencia_principal=%u,sequencia_referencia=%u\n",
                    pendingMain.receivedSequence, trueSequence);
    }
  }

  recordResult(technique, mode, trueSequence, status, payloadMatches,
               residue, hasVerification, verificationUs);
  pendingMain = PendingMain();
  if (trueSequence == 999) {
    runActive = false;
    printOneTable(technique, mode);
  }
}

void processByte(uint8_t value) {
  lastParserByteUs = micros();
  switch (parserState) {
    case SEARCH_MARKER:
      if (value == SOF) {
        const uint32_t nowUs = micros();
        if (havePreviousMainMarker) addTiming(rxIntervalStats, nowUs - previousMainMarkerUs);
        previousMainMarkerUs = nowUs;
        havePreviousMainMarker = true;
        parserState = READ_MAIN_LENGTH;
      } else if (value == DEBUG_SOF) {
        parserState = READ_DEBUG_LENGTH;
      }
      break;

    case READ_MAIN_LENGTH:
      if (value != CRC_BODY_LENGTH) {
        markMalformedMain();
        resetParser();
      } else {
        mainBytes[0] = value;
        expectedBodyLength = value;
        bodyIndex = 0;
        parserState = READ_MAIN_BODY;
      }
      break;

    case READ_MAIN_BODY:
      mainBytes[1 + bodyIndex++] = value;
      if (bodyIndex == expectedBodyLength) {
        evaluateMain();
        resetParser();
      }
      break;

    case READ_DEBUG_LENGTH:
      if (value != DEBUG_BODY_LENGTH) {
        resetParser();
      } else {
        debugBytes[0] = value;
        expectedBodyLength = value;
        bodyIndex = 0;
        parserState = READ_DEBUG_BODY;
      }
      break;

    case READ_DEBUG_BODY:
      debugBytes[1 + bodyIndex++] = value;
      if (bodyIndex == expectedBodyLength) {
        evaluateDebug();
        resetParser();
      }
      break;
  }
}

void prepareNewRun(bool clearTables) {
  if (clearTables) {
    for (uint8_t technique = 0; technique < 2; ++technique) {
      for (uint8_t mode = 0; mode < 4; ++mode) tableStats[technique][mode] = ResultStats();
    }
  }
  resetParser();
  pendingMain = PendingMain();
  memset(receivedFrameAvailable, 0, sizeof(receivedFrameAvailable));
  readingFrameSequence = false;
  haveExpectedSequence = false;
  expectedSequence = 0;
  waitingForRecovery = false;
  runActive = false;
  lastReferenceFrameUs = 0;
  rxIntervalStats = TimingStats();
  havePreviousMainMarker = false;
  while (Serial2.available()) Serial2.read();
  Serial.printf("RX_PRONTO,nova_rodada,tabelas=%s\n", clearTables ? "LIMPAS" : "MANTIDAS");
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(BAUD_RATE, SERIAL_8N1, RX2_PIN, TX2_PIN);
  delay(500);
  Serial.println("tecnica,modo,sequencia,residuo,decisao,payload_confere,verificacao_us,recuperacao_us");
  Serial.println("RX pronto: r=nova rodada, t=tabelas, x=limpar tudo, p<seq>=quadro recebido");
  prepareNewRun(true);
}

void loop() {
  while (Serial2.available()) processByte(static_cast<uint8_t>(Serial2.read()));

  if (parserState != SEARCH_MARKER && micros() - lastParserByteUs > INSIDE_FRAME_TIMEOUT_US) {
    if (parserState == READ_MAIN_LENGTH || parserState == READ_MAIN_BODY) markMalformedMain();
    Serial.println("AVISO,timeout_dentro_do_quadro");
    resetParser();
  }

  if (runActive && expectedSequence < 1000 &&
      micros() - lastReferenceFrameUs > INTERFRAME_TIMEOUT_US) {
    recordResult(lastReferenceTechnique, lastReferenceMode, expectedSequence,
                 STATUS_LOST, false, 0, false, 0);
    expectedSequence = static_cast<uint16_t>(expectedSequence + 1);
    lastReferenceFrameUs = micros();
    Serial.println("AVISO,timeout_entre_quadros");
  }

  while (Serial.available()) {
    const char command = static_cast<char>(Serial.read());
    if (readingFrameSequence) {
      if (command >= '0' && command <= '9') {
        if (!invalidFrameSequence) {
          const uint32_t nextSequence = requestedFrameSequence * 10UL + (command - '0');
          if (nextSequence >= FRAMES_PER_RUN) invalidFrameSequence = true;
          else requestedFrameSequence = static_cast<uint16_t>(nextSequence);
        }
        haveFrameSequenceDigit = true;
      } else if (command == '\n' || command == '\r') {
        if (haveFrameSequenceDigit && !invalidFrameSequence) printReceivedFrame(requestedFrameSequence);
        else Serial.println("ERRO,uso=p<sequencia_de_0_a_999> e Enter");
        readingFrameSequence = false;
      } else {
        invalidFrameSequence = true;
      }
      continue;
    }
    if (command == 'p' || command == 'P') {
      readingFrameSequence = true;
      haveFrameSequenceDigit = false;
      invalidFrameSequence = false;
      requestedFrameSequence = 0;
      continue;
    }
    if (command == 'r' || command == 'R') prepareNewRun(false);
    else if (command == 'x' || command == 'X') prepareNewRun(true);
    else if (command == 't' || command == 'T') printAllTables();
  }
}
