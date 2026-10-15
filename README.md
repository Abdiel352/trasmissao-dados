# Telemetria ESP32 - CRC-16 com injetor de ruido

Este projeto tem tres programas independentes. Grave cada um em uma placa ESP32 e mantenha as tres placas alimentadas por USB. Depois de gravado, cada ESP32 executa seu proprio programa automaticamente; os tres funcionam juntos pelo circuito UART.

## Arquivos

| Arquivo | Placa | Funcao |
|---|---|---|
| `tx/tx.ino` | ESP32 TX | Envia 1000 quadros quando recebe `r` no Monitor Serial. |
| `rx/rx.ino` | ESP32 RX | Recebe, confere o CRC-16/CCITT e escreve um CSV no Monitor Serial. |
| `noise/noise.ino` | ESP32 de ruido | Escuta a UART e aciona o transistor conforme o modo escolhido. |

## Ligacoes

Use os nomes escritos na propria placa:

```text
TX (placa transmissora)                         RX (placa receptora)
TX2 -- resistor de 1 kOhm --+------------------- RX2
                              |
                              +------------------- RX2 da placa de ruido
                              |
                              +------------------- coletor do 2N2222

D18 da placa de ruido -- resistor de 1 kOhm -- base do 2N2222
base do 2N2222 -- resistor de 10 kOhm -- GND
emissor do 2N2222 --------------------- GND comum

GND do TX, GND do RX e GND do ESP32 de ruido: todos unidos.
```

Nao una os pinos `3V3` das placas. Cada placa pode receber energia pelo proprio USB. Nao use `TX0` e `RX0`, pois eles pertencem ao USB/Monitor Serial.

## Como gravar no Arduino IDE

1. Instale o pacote **esp32 by Espressif Systems** no Gerenciador de Placas.
2. Selecione a placa `ESP32 Dev Module` e a porta USB correta.
3. Abra `tx/tx.ino` e envie para a placa que sera o TX.
4. Abra `rx/rx.ino` e envie para a placa que sera o RX.
5. Abra `noise/noise.ino` e envie para a placa que sera o injetor de ruido.
6. Mantenha todas conectadas, faca as ligacoes acima e abra tres Monitores Seriais, um para cada porta, em **115200 baud**.

## Ordem para executar uma rodada

1. No Monitor Serial do RX, espere aparecer o cabecalho CSV.
2. No Monitor Serial do ESP32 de ruido, digite `0`, `1`, `2` ou `3` para definir o modo e envie. Em seguida digite `r` para reiniciar a sequencia prevista em zero.
3. No Monitor Serial do TX, digite `r`. Ele enviara exatamente 1000 quadros, com intervalo de 20 ms.
4. Copie as linhas que comecam com `CSV,` no Monitor Serial do RX para um arquivo `.csv` ou planilha.

## Modos de ruido

| Comando | Modo | Acao |
|---:|---|---|
| `0` | Desligado | Nenhum pulso e injetado. |
| `1` | Um bit | Um pulso de aproximadamente um periodo de bit dentro do payload. |
| `2` | Dois bits | Dois pulsos em posicoes aleatorias do payload. |
| `3` | Rajada | Uma rajada contigua de 3 a 8 bits, dentro de um byte do payload. |

O injetor usa `esp_timer`; o codigo nao usa `delayMicroseconds()` para os pulsos. Como o transistor so puxa a linha para baixo, o log indica se o bit escolhido era `1` (`mudou=SIM`) ou ja era `0` (`mudou=NAO`).

## Explicacao dos codigos

### `tx/tx.ino` - transmissor

O transmissor nao inicia o experimento sozinho. No Monitor Serial dele, digite `r` e envie. A funcao `runExperiment()` manda os 1000 quadros, um a cada 20 ms.

Para cada numero de sequencia, `makePayload()` monta quatro bytes de telemetria simulada: tensao em milivolts e temperatura multiplicada por 100. Os valores sao propositalmente previsiveis; assim, o ESP32 de ruido consegue saber se o bit que ele tentou puxar era `1` ou ja era `0`.

A funcao `sendFrame()` monta os bytes abaixo, calcula o CRC-16/CCITT apenas sobre a sequencia e o payload, e envia tudo em `Serial2` (TX2):

```text
SOF A5 | sequencia (2) | payload (4) | CRC (2)
```

Em seguida ela envia a copia de depuracao. Essa copia e a verdade de referencia do experimento; ela nao participa da decisao do CRC.

```cpp
Serial2.write(frame, sizeof(frame));       // quadro que sera testado pelo CRC
Serial2.write(debugCopy, sizeof(debugCopy)); // referencia posterior
```

### `rx/rx.ino` - receptor

O receptor fica lendo `Serial2` pelo RX2 sem parar. Ele funciona como uma pequena maquina de estados:

1. `WAIT_SOF`: procura o byte `A5`, marcador de inicio.
2. `READ_FRAME`: junta os oito bytes restantes do quadro protegido.
3. `READ_DEBUG`: le os sete bytes da copia de depuracao que vem depois.

Quando o quadro protegido fica completo, o RX calcula novamente o CRC dos seis bytes protegidos e compara esse resultado com os dois bytes de CRC recebidos:

```cpp
crcPassed = crc16Ccitt(&frame[1], 6) == receivedCrc;
```

Somente depois de tomar essa decisao ele compara o payload recebido com a copia de depuracao. Daqui saem os tres resultados principais:

| Resultado | Significado |
|---|---|
| `OK` | CRC passou e o payload recebido confere com a copia verdadeira. |
| `CORROMPIDO` | CRC falhou; o RX descarta o quadro. |
| `NAO_DETECTADO` | CRC passou, mas o payload recebido difere da copia verdadeira. Este e o erro central do trabalho. |

O RX tambem possui timeout de 30 ms dentro de um quadro incompleto e verifica a sequencia dos quadros aceitos. Uma lacuna vira `PERDIDO`. Cada resultado e escrito como uma linha iniciada por `CSV,`; copie essas linhas para uma planilha ao fim da rodada.

### `noise/noise.ino` - injetor de ruido

Esse ESP32 nao envia bytes pela UART. O RX2 dele so observa a mesma linha que chega ao receptor. Quando ele reconhece o marcador `A5`, `planInjection()` sorteia onde atuar no payload do quadro atual.

O pino `D18` vai para a base do transistor. Quando o codigo o coloca em nivel alto, o transistor conduz e puxa a linha UART para `0`. Portanto, ele so consegue transformar um bit `1` em `0`; puxar um bit que ja era `0` nao produz alteracao.

Os temporizadores sao preparados em `armTimers()` e os callbacks fazem o pulso:

```cpp
startPulse()  // D18 vai para HIGH: transistor puxa a linha para 0
endPulse()    // D18 volta para LOW: transistor para de atuar
```

Antes de cada rodada, no Monitor Serial do injetor, envie o modo desejado (`0`, `1`, `2` ou `3`) e depois `r`. O `r` zera a sequencia prevista pelo injetor para ela coincidir com o TX, que sempre inicia pelo quadro 0.

### O que cada `Serial` faz

| Nome no codigo | Para onde vai | Uso |
|---|---|---|
| `Serial` | USB, pinos TX0/RX0 | Monitor Serial, comandos e logs. |
| `Serial2` | TX2/RX2 | Comunicacao de telemetria entre as placas. |

## Protocolo implementado

Cada quadro enviado pela UART tem tamanho fixo:

```text
A5 | sequencia (2 bytes) | payload (4 bytes) | CRC-16/CCITT (2 bytes)
```

O CRC protege a sequencia e o payload. Logo depois, o TX envia uma copia de depuracao fora da regiao protegida:

```text
5A | sequencia (2 bytes) | payload verdadeiro (4 bytes)
```

O RX decide **somente** pelo CRC. Apenas depois ele le a copia e classifica o quadro como `OK`, `CORROMPIDO` ou `NAO_DETECTADO`. Tambem ha timeout de 30 ms dentro do quadro e acompanhamento de lacunas de sequencia.

## Observacao importante sobre precisao

Em 115200 baud, um bit dura 8,68 us. A implementacao usa `esp_timer`, como pedido no enunciado, mas a precisao final depende da versao do core ESP32, carga de Wi-Fi/Bluetooth e da placa. Desative Wi-Fi/Bluetooth, mantenha apenas este programa em execucao e valide os pulsos com um analisador logico antes da demonstracao. Para resultados mais rigorosos, o proximo aperfeicoamento e migrar o pulso para um timer de hardware/RMT.
