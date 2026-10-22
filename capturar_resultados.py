"""Captura as tres UARTs USB e salva os resultados em resultados/*.csv.

Os ESP32 nao conseguem escrever diretamente no disco do computador. Este
programa substitui os tres Monitores Seriais enquanto a captura estiver ativa.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
import sys
import threading


RESULTADOS = Path(__file__).resolve().parent / "resultados"
TX_FRAMES_PER_RUN = 1000
HEADERS = {
    "tx": ["capturado_em", "registro", "tecnica", "modo", "sequencia", "quadro_hex", "detalhes"],
    "rx": [
        "capturado_em", "registro", "tecnica", "modo", "sequencia", "residuo",
        "decisao", "payload_confere", "verificacao_us", "recuperacao_us", "detalhes",
    ],
    "noise": [
        "capturado_em", "registro", "sequencia", "modo", "byte", "bits",
        "tempo_us", "duracao_us", "linha_alta", "mascara_prevista", "detalhes",
    ],
}


def key_values(parts: list[str]) -> dict[str, str]:
    return dict(part.split("=", 1) for part in parts if "=" in part)


def parse_record(board: str, line: str, timestamp: str) -> list[str] | None:
    """Converte somente registros relevantes; preserva a linha original."""
    parts = line.strip().split(",")
    kind = parts[0]
    if board == "tx":
        if kind not in {"CONFIG", "TX_RUN", "TX_METRICAS", "QUADRO_ORIGINAL", "SELFTEST"}:
            return None
        values = key_values(parts[1:])
        return [
            timestamp, kind, values.get("tecnica", ""), values.get("modo", ""),
            values.get("seq", ""), values.get("hex", ""), line,
        ]

    if board == "rx":
        if kind == "CSV" and len(parts) == 9:
            return [timestamp, kind, *parts[1:9], ""]
        if kind not in {"TABELA", "RX_INTERVALO", "QUADRO_RECEBIDO", "AVISO", "RX_PRONTO"}:
            return None
        values = key_values(parts[1:])
        technique = parts[1] if kind == "TABELA" and len(parts) > 1 else ""
        return [
            timestamp, kind, technique, values.get("modo", ""),
            values.get("seq_referencia", values.get("seq", "")),
            "", "", "", "", "", line,
        ]

    if board == "noise":
        if kind == "INJECAO" and len(parts) == 9:
            return [timestamp, kind, *parts[1:9], ""]
        if kind not in {"NOISE", "NOISE_STATUS"}:
            return None
        values = key_values(parts[1:])
        return [
            timestamp, kind, "", values.get("modo", ""), "", "", "", "", "", "", line,
        ]

    raise ValueError(f"Placa desconhecida: {board}")


def open_csv(path: Path, header: list[str], *, erase: bool = False):
    path.parent.mkdir(parents=True, exist_ok=True)
    empty = erase or not path.exists() or path.stat().st_size == 0
    handle = path.open("w" if erase else "a", encoding="utf-8", newline="")
    writer = csv.writer(handle)
    if empty:
        writer.writerow(header)
        handle.flush()
    return handle, writer


@dataclass
class Board:
    name: str
    port: str
    serial: object
    path: Path
    handle: object
    writer: object
    records: int = 0
    lock: threading.RLock = field(default_factory=threading.RLock)
    exporting: bool = False
    requested_sequence: int = -1
    original_event: threading.Event = field(default_factory=threading.Event)


def main() -> int:
    parser = argparse.ArgumentParser(description="Salva TX, RX e ruido em resultados/*.csv")
    parser.add_argument("--tx", help="Porta USB do transmissor, por exemplo COM3")
    parser.add_argument("--rx", help="Porta USB do receptor, por exemplo COM5")
    parser.add_argument("--noise", help="Porta USB do gerador, por exemplo COM4")
    parser.add_argument("--list", action="store_true", help="Lista as portas seriais e sai")
    args = parser.parse_args()

    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        print("Falta pyserial. Instale com: py -m pip install -r requirements.txt", file=sys.stderr)
        return 1

    if args.list:
        for item in list_ports.comports():
            print(f"{item.device}: {item.description}")
        return 0
    if not all((args.tx, args.rx, args.noise)):
        parser.error("informe --tx, --rx e --noise (ou use --list)")
    ports = {"tx": args.tx, "rx": args.rx, "noise": args.noise}
    if len({port.upper() for port in ports.values()}) != 3:
        parser.error("cada placa precisa de uma porta COM diferente")

    boards: dict[str, Board] = {}
    stop = threading.Event()
    workers: list[threading.Thread] = []
    export_workers: list[threading.Thread] = []

    def export_originals(board: Board) -> None:
        saved = 0
        missing: list[int] = []
        try:
            for sequence in range(TX_FRAMES_PER_RUN):
                if stop.is_set():
                    break
                received = False
                for _attempt in range(2):
                    with board.lock:
                        board.requested_sequence = sequence
                        board.original_event.clear()
                        board.serial.write(f"p{sequence}\n".encode("ascii"))
                    if board.original_event.wait(timeout=2):
                        received = True
                        break
                if received:
                    saved += 1
                    if saved % 100 == 0:
                        print(f"\n[TX] {saved}/{TX_FRAMES_PER_RUN} quadros originais salvos", flush=True)
                else:
                    missing.append(sequence)
        except (serial.SerialException, OSError) as exc:
            print(f"\n[TX] Exportacao interrompida: {exc}", file=sys.stderr, flush=True)
        finally:
            with board.lock:
                board.exporting = False
                board.requested_sequence = -1
            if missing:
                print(f"\n[TX] Faltaram {len(missing)} quadros: {missing}", flush=True)
            print(f"\n[TX] Exportacao encerrada: {saved}/{TX_FRAMES_PER_RUN} quadros originais recebidos", flush=True)

    def start_export(board: Board) -> None:
        with board.lock:
            if board.exporting:
                print("\n[TX] A exportacao dos originais ja esta em andamento.", flush=True)
                return
            board.exporting = True
        print("\n[TX] Pedindo os 1000 quadros originais apos a rodada...", flush=True)
        worker = threading.Thread(target=export_originals, args=(board,), daemon=True)
        worker.start()
        export_workers.append(worker)

    def read_board(board: Board) -> None:
        while not stop.is_set():
            try:
                # A leitura bloqueante fica fora do lock: a thread de exportacao
                # precisa poder enviar p<seq> enquanto esta thread aguarda a resposta.
                raw = board.serial.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                stamp = datetime.now().astimezone().isoformat(timespec="milliseconds")
                row = parse_record(board.name, line, stamp)
                with board.lock:
                    if row is not None:
                        board.writer.writerow(row)
                        board.handle.flush()
                        board.records += 1
                    if board.name == "tx" and line.startswith("QUADRO_ORIGINAL,"):
                        values = key_values(line.split(",")[1:])
                        if values.get("seq") == str(board.requested_sequence):
                            board.original_event.set()
            except (serial.SerialException, OSError) as exc:
                if not stop.is_set():
                    print(f"\nERRO na porta {board.port}: {exc}", file=sys.stderr, flush=True)
                    stop.set()
                return
            if line.startswith("TX_RUN,FIM") and board.name == "tx":
                print(f"\n[{board.name.upper()}] {line}", flush=True)
                start_export(board)
            elif not line.startswith(("CSV,", "INJECAO,", "QUADRO_ORIGINAL,")):
                print(f"\n[{board.name.upper()}] {line}", flush=True)
            elif not line.startswith("QUADRO_ORIGINAL,") and board.records % 100 == 0:
                print(f"\n[{board.name.upper()}] {board.records} registros salvos", flush=True)

    def erase_board(board: Board) -> None:
        with board.lock:
            board.serial.reset_input_buffer()
            board.handle.close()
            board.handle, board.writer = open_csv(board.path, HEADERS[board.name], erase=True)
            board.records = 0
        print(f"Apagado: {board.path}")

    try:
        # Abre primeiro as portas. Se alguma estiver ocupada pelo Arduino IDE,
        # os arquivos existentes permanecem intocados.
        for name, port in ports.items():
            connection = serial.Serial(port, 115200, timeout=0.2, write_timeout=2)
            boards[name] = Board(name, port, connection, RESULTADOS / f"{name}.csv", None, None)
        for board in boards.values():
            board.handle, board.writer = open_csv(board.path, HEADERS[board.name])
            worker = threading.Thread(target=read_board, args=(board,), daemon=True)
            worker.start()
            workers.append(worker)

        print(f"Capturando em {RESULTADOS}")
        print("Comandos: tx/rx/noise <comando>, tx exportar, del [tx|rx|noise], ajuda, sair")
        print("Feche os Monitores Seriais do Arduino IDE antes de usar este programa.")
        while not stop.is_set():
            try:
                command = input("> ").strip()
            except (EOFError, KeyboardInterrupt):
                break
            if not command:
                continue
            words = command.split(maxsplit=1)
            action = words[0].lower()
            argument = words[1].strip() if len(words) > 1 else ""
            if action in {"sair", "exit", "quit"}:
                break
            if action in {"ajuda", "help"}:
                print("Exemplos: tx c | tx 0 | noise 0 | noise r | rx r | tx r | del | del rx")
                print("del limpa os tres CSVs; del <placa> limpa somente um. Use com a rodada parada.")
                print("rx x limpa a tabela do ESP32 e o arquivo rx.csv; rx r preserva ambos.")
                print("Apos TX_RUN,FIM, os 1000 quadros originais sao pedidos automaticamente.")
                print("tx exportar pede os originais novamente, sem transmitir quadros pela UART2.")
                continue
            if action == "del" and (not argument or argument.lower() in boards):
                targets = [boards[argument.lower()]] if argument else list(boards.values())
                if any(board.exporting for board in targets):
                    print("Aguarde o fim da exportacao dos originais antes de apagar o CSV do TX.")
                    continue
                for board in targets:
                    erase_board(board)
                continue
            if action in boards and argument:
                board = boards[action]
                if argument.lower() == "del":
                    if board.exporting:
                        print("Aguarde o fim da exportacao dos originais antes de apagar o CSV do TX.")
                        continue
                    erase_board(board)
                    continue
                if action == "tx" and argument.lower() == "exportar":
                    start_export(board)
                    continue
                if board.exporting:
                    print("Aguarde a exportacao dos originais antes de enviar outro comando ao TX.")
                    continue
                if action == "rx" and argument.lower() == "x":
                    erase_board(board)
                try:
                    board.serial.write((argument + "\n").encode("ascii"))
                except (UnicodeEncodeError, serial.SerialException, OSError) as exc:
                    print(f"Nao foi possivel enviar para {board.port}: {exc}", file=sys.stderr)
                continue
            print("Comando invalido. Digite ajuda.")
    except (serial.SerialException, OSError) as exc:
        print(f"Nao foi possivel iniciar a captura: {exc}", file=sys.stderr)
        return 1
    finally:
        stop.set()
        for board in boards.values():
            board.original_event.set()
        for worker in export_workers:
            worker.join(timeout=2)
        for worker in workers:
            worker.join(timeout=1)
        for board in boards.values():
            if board.handle is not None:
                board.handle.close()
            board.serial.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
