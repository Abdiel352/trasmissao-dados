# Telemetria ESP32 com ruido real

Projeto com tres ESP32 para comparar **CRC-16/CCITT-FALSE** e **Fletcher-16** em uma UART de 115200 baud. Cada pasta e um sketch independente do Arduino IDE.

## Arquivos

| Arquivo | Gravar em | Funcao |
|---|---|---|
| `tx/tx.ino` | ESP32 TX | Monta os quadros, calcula o codigo de deteccao e envia 1000 quadros. |
| `rx/rx.ino` | ESP32 RX | Verifica o residuo, classifica cada quadro, gera CSV e tabelas. |
| `noise/noise.ino` | ESP32 de ruido | Detecta cada quadro e aciona o transistor nos modos 0 a 3. |
| `ESPECIFICACAO.md` | Relatorio | Especificacao completa do protocolo e das decisoes do RX. |
| `resultados/modelo_tabela.csv` | Planilha | Modelo para reunir os resultados finais. |

## Ligacoes

```text
TX2 do ESP32 TX -- resistor 1 kOhm --+-- RX2 do ESP32 RX
                                      +-- RX2 do ESP32 de ruido
                                      +-- coletor do transistor NPN

D18 do ESP32 de ruido -- resistor 1 kOhm -- base do NPN
base do NPN ---------- resistor 10 kOhm --- GND
emissor do NPN ----------------------------- GND

GND do TX -------- GND do RX -------- GND do ESP32 de ruido
```

Cada ESP32 pode continuar alimentado pelo proprio USB. Nao una `3V3` ou `VIN`. Nao use `TX0/RX0` na montagem: eles ficam livres para o Monitor Serial.

## Gravacao no Arduino IDE

1. Instale `esp32 by Espressif Systems` no Gerenciador de Placas.
2. Selecione `ESP32 Dev Module` e a porta da placa.
3. Grave `tx/tx.ino` no TX, `rx/rx.ino` no RX e `noise/noise.ino` no injetor.
4. Abra os Monitores Seriais em **115200 baud**.
5. Ao iniciar, o TX deve imprimir `SELFTEST,CRC=OK,FLETCHER=OK`.

## Comandos

### TX

| Comando | Acao |
|---|---|
| `c` | Seleciona CRC-16/CCITT-FALSE. |
| `f` | Seleciona Fletcher-16. |
| `0`, `1`, `2`, `3` | Grava no quadro qual modo esta sendo testado. |
| `r` | Envia uma rodada de 1000 quadros. |
| `?` | Mostra a configuracao atual. |

### RX

| Comando | Acao |
|---|---|
| `x` | Limpa todas as tabelas e prepara a primeira rodada. |
| `r` | Prepara a proxima rodada sem apagar as tabelas anteriores. |
| `t` | Imprime todas as tabelas acumuladas. |

### ESP32 de ruido

| Comando | Acao |
|---|---|
| `0` | Sem ruido. |
| `1` | Um bit aleatorio do payload por quadro. |
| `2` | Dois bits aleatorios distintos por quadro. |
| `3` | Rajada contigua de 3 a 8 bits dentro de um byte. |
| `k` | Demonstracao construida para quebrar Fletcher-16. |
| `r` | Zera a sequencia prevista e reinicia o parser. |
| `+` / `-` | Ajusta o instante do pulso em 1 us para calibracao no analisador logico. |
| `?` | Mostra modo, sequencia e calibracao. |

Quebra de linha enviada pelo Monitor Serial e ignorada; pode deixar `Nova linha` selecionado.

## Ordem correta de cada rodada

Na primeira rodada apenas, envie `x` ao RX. Nas seguintes, envie `r` ao RX.

Depois, para cada combinacao:

1. No TX, selecione a tecnica (`c` ou `f`) e envie o numero do modo (`0` a `3`).
2. No ESP de ruido, envie o mesmo numero de modo e depois `r`.
3. Confira que o RX respondeu `RX_PRONTO`.
4. No TX, envie `r` para comecar os 1000 quadros.
5. Espere `TX_RUN,FIM`. O RX imprime uma linha `TABELA` quando recebe a sequencia 999.

Rodadas exigidas:

```text
CRC-16:     modos 0, 1, 2 e 3
Fletcher-16: modos 1, 2 e 3
```

O baseline do Fletcher-16 e opcional pelo enunciado, mas o programa tambem o aceita.

## O que aparece no RX

Cada linha por quadro tem o formato:

```text
CSV,tecnica,modo,sequencia,residuo,decisao,payload_confere,verificacao_us,recuperacao_us
```

As decisoes sao:

- `OK`: a checagem passou e o payload confere.
- `CORROMPIDO`: o residuo nao foi zero; o payload e descartado.
- `PERDIDO`: o RX nao conseguiu concluir a checagem por marcador, comprimento ou timeout.
- `NAO_DETECTADO`: o residuo foi zero, mas a copia verdadeira mostrou payload diferente.

A linha `TABELA` informa as quatro contagens, o total, a taxa de nao deteccao, o tempo medio de verificacao e o tempo medio de recuperacao. As quatro contagens devem somar 1000 por rodada.

## Metricas produzidas

- TX: tempo minimo, medio e maximo de calculo do codigo.
- TX e RX: intervalo observado entre quadros.
- RX: tempo de verificacao por quadro.
- RX: tempo de recuperacao depois de `PERDIDO`.
- Protocolo: 14 bytes no quadro principal, sendo 10 bytes de overhead para 4 bytes de payload.
- RX: taxa `NAO_DETECTADO / (CORROMPIDO + NAO_DETECTADO)`.

A copia de depuracao tem 12 bytes, mas nao entra no calculo de overhead, conforme o enunciado.

## Demonstracao de erro nao detectado

Para produzir um caso real e explicavel:

1. No RX envie `r`.
2. No TX envie `f`, depois `3`.
3. No ESP de ruido envie `k`, depois `r`.
4. No TX envie `r`.

O modo especial espera um payload contendo `0xFF` e aplica uma rajada de 8 bits, transformando-o em `0x00`. No Fletcher-16, ambos representam zero modulo 255, portanto o residuo pode continuar `0000` e o RX registra `NAO_DETECTADO`. Repita com `c` no TX para mostrar que o CRC detecta o mesmo padrao.

## Calibracao obrigatoria

Um bit em 115200 baud dura aproximadamente 8,68 us. O injetor usa `esp_timer`, nunca `delayMicroseconds()`, registra o timestamp real e le RX2 imediatamente antes do pulso para saber se a linha estava alta. Mesmo assim, a latencia da UART e do sistema varia entre placas. Antes das rodadas oficiais, observe TX2 e D18 num analisador logico e use `+` ou `-` no injetor ate o pulso cair no centro do bit escolhido.

O timeout interno do RX e 3000 us. Um quadro principal de 14 bytes ocupa aproximadamente 1215 us na UART; 3000 us oferece margem. O timeout entre quadros e 50000 us, mais de duas vezes o intervalo normal pouco acima de 20 ms. Os logs `TX_METRICAS` e `RX_INTERVALO` fornecem a medicao usada na justificativa final.
