# Especificacao do protocolo

## Enlace

- UART2, 115200 baud, 8N1, logica de 3,3 V.
- Somente `TX -> RX`; nao ha ACK nem retransmissao.
- `TX2` do transmissor e ligado a `RX2` do receptor por resistor de 1 kOhm.
- O injetor observa a mesma linha por `RX2` e a puxa para zero por um transistor comandado por `D18`.

## Quadro principal

Todos os campos numericos de mais de um byte usam ordem big-endian.

| Campo | Bytes | Protegido | Descricao |
|---|---:|---|---|
| SOF | 1 | Nao | Marcador `0xA5`. |
| LENGTH | 1 | Sim | Quantidade de bytes depois de LENGTH: sempre 12. |
| VERSION | 1 | Sim | Versao 1. |
| TECHNIQUE | 1 | Sim | 1=CRC-16/CCITT-FALSE; 2=Fletcher-16. |
| NOISE_MODE | 1 | Sim | Modo declarado: 0 a 3. |
| SEQUENCE | 2 | Sim | Contador modulo 65536. |
| PAYLOAD_LENGTH | 1 | Sim | Valor 4. |
| PAYLOAD | 4 | Sim | Tensao em mV e temperatura x100. |
| DETECTION | 2 | Campo de checagem | CRC ou bytes de verificacao Fletcher. |

Tamanho total: 14 bytes. Payload: 4 bytes. Overhead: 10 bytes.

O campo de comprimento foi escolhido em vez de marcador de fim porque permite que o payload contenha qualquer byte sem escape. Se o comprimento nao for 12, o RX abandona o quadro e procura o proximo marcador.

## Tecnicas

### CRC-16/CCITT-FALSE

- Polinomio: `0x1021`.
- Valor inicial: `0xFFFF`.
- Sem reflexao e sem XOR final.
- Ordem do CRC no quadro: byte alto, depois byte baixo.
- O TX calcula sobre `LENGTH` ate o ultimo byte do payload.
- O RX processa os mesmos campos e o CRC recebido; quadro valido produz residuo `0x0000`.

### Fletcher-16

- `sum1` e `sum2` sao calculados modulo 255.
- O TX anexa dois bytes construidos para zerar os dois acumuladores.
- O RX processa campos protegidos e os dois bytes recebidos; quadro valido produz residuo `0x0000`.

Somente a tecnica muda entre as rodadas. Tamanho, campos, payload, ritmo e tratamento permanecem iguais.

## Copia de depuracao

Enviada imediatamente depois do quadro principal:

```text
5A | LENGTH=10 | VERSION | TECHNIQUE | MODE | SEQUENCE(2) | PAYLOAD_LENGTH | PAYLOAD(4)
```

Ela nunca participa da decisao. Primeiro o RX conclui a checagem do quadro principal; somente depois le a copia e confere o payload. O injetor pula toda essa regiao.

## Decisao e ressincronizacao do RX

1. Procura `0xA5`.
2. Le LENGTH; valor diferente de 12 torna o quadro inconclusivo.
3. Le exatamente LENGTH bytes.
4. Verifica estrutura e calcula o residuo incluindo DETECTION.
5. Residuo zero aceita; residuo diferente descarta sem entregar o payload.
6. Le a copia de depuracao e classifica o resultado.
7. Volta a procurar o proximo marcador a partir do byte seguinte ao quadro/copia consumido. Depois de comprimento invalido ou timeout, volta imediatamente ao estado de busca.

Nao existe retransmissao: o TX continua com a proxima sequencia.

## Classificacao

- `OK`: checagem aceitou e payload confere.
- `CORROMPIDO`: checagem terminou com residuo diferente de zero.
- `PERDIDO`: marcador, comprimento ou timeout impediram concluir a checagem.
- `NAO_DETECTADO`: checagem aceitou, mas o payload difere da copia verdadeira.

Taxa de nao deteccao:

```text
NAO_DETECTADO / (CORROMPIDO + NAO_DETECTADO)
```

## Sequencia

O numero esperado apos `n` e `(n + 1) modulo 65536`. A diferenca tambem e calculada em 16 bits:

- diferenca zero: ordem correta;
- diferenca de 1 a 32767: lacuna; a diferenca indica quantos quadros faltaram;
- diferenca de 32768 a 65535: repeticao ou quadro fora de ordem; registra aviso sem contar novamente.

Uma nova rodada comeca em sequencia zero. O comando `r` do RX limpa o estado da rodada, mas preserva tabelas anteriores.

## Timeouts

- **Dentro do quadro:** 3000 us, reiniciado a cada byte. Em 115200 baud, 14 bytes 8N1 levam aproximadamente 1215 us. O valor oferece margem, mas nao deixa o parser preso ate o quadro seguinte.
- **Entre quadros:** 50000 us, contado depois da ultima copia de referencia concluida. O TX espera 20 ms depois de transmitir cada par quadro+copia, portanto o intervalo observado fica pouco acima de 20 ms. Cinquenta mil microssegundos toleram mais de dois intervalos normais sem atrasar excessivamente a deteccao.

Se o proximo quadro chegar antes do timeout com uma lacuna de sequencia, a lacuna e contabilizada e o timeout nao conta o mesmo quadro novamente. O intervalo real e registrado por `micros()` no TX e no RX para justificar ou ajustar os valores.

## Ruido

- Modo 0: desligado.
- Modo 1: um bit aleatorio em cada payload.
- Modo 2: dois bits aleatorios distintos em cada payload.
- Modo 3: rajada contigua de 3 a 8 bits dentro de um byte do payload.
- O transistor so transforma `1` em `0`. Imediatamente antes de acionar o NPN, o injetor le o nivel real pelo proprio RX2 e registra se a linha estava alta; no modo de um bit, isso prova diretamente se houve mudanca.
- Cada log contem sequencia, modo, posicao, timestamp real do pulso, duracao, nivel real antes da injecao e mascara prevista dos bits que mudam.
- A rajada permanece dentro de um unico byte e dura no maximo 8 periodos, menos que os 10 periodos de um byte UART completo.

O modo adicional `k` existe apenas para a demonstracao “quebrar o protocolo”: converte um byte `0xFF` em `0x00`. Fletcher-16 trabalha modulo 255 e pode nao detectar essa mudanca; CRC-16 detecta.

