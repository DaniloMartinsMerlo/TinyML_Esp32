"""
receiver.py — Recebe WAV de 60s do ESP32 pelo serial

Uso:
    pip install pyserial
    python receiver.py --port /dev/ttyUSB0 --out gravacao.wav
    python receiver.py --port COM3 --out gravacao.wav   (Windows)

IMPORTANTE: feche o Arduino Serial Monitor antes de rodar.
"""

import serial
import sys
import argparse
import time
import os

BAUD_RATE   = 115200
SAMPLE_RATE = 16000
SECONDS     = 60
# Total de bytes que esperamos receber (header WAV + amostras)
WAV_HEADER  = 44
AUDIO_BYTES = SAMPLE_RATE * SECONDS * 2   # 1.920.000 bytes
TOTAL_BYTES = WAV_HEADER + AUDIO_BYTES    # 1.920.044 bytes

def progress_bar(received, total, speed_kbs):
    pct = received / total
    bar = "█" * int(pct * 20)
    pad = " " * (20 - len(bar))
    eta = (total - received) / (speed_kbs * 1024) if speed_kbs > 0 else 0
    print(f"\r  [{bar}{pad}] {pct*100:.0f}%  "
          f"{received/1024:.0f}/{total/1024:.0f} KB  "
          f"{speed_kbs:.0f} KB/s  "
          f"ETA {eta:.0f}s   ", end="", flush=True)

def receive(port, out_path):
    print(f"Conectando em {port} @ {BAUD_RATE}bps...")
    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=5)
    except serial.SerialException as e:
        print(f"ERRO ao abrir porta: {e}")
        sys.exit(1)

    # Limpa o buffer de entrada (pode ter lixo do boot do ESP32)
    time.sleep(0.5)
    ser.reset_input_buffer()

    print("Conectado!")
    print("Aguardando ##READY## do ESP32...\n")

    # Aguarda o ESP32 estar pronto
    ready_timeout = time.time() + 10
    while time.time() < ready_timeout:
        line = ser.readline().decode("utf-8", errors="ignore").strip()
        if line:
            print(f"  [ESP32] {line}")
        if "READY" in line:
            break
    else:
        print("AVISO: ##READY## não recebido, continuando mesmo assim...")

    print("\nPressione o botão no ESP32 para começar a gravar.")
    print("Aguardando ##REC_START##...\n")

    # Aguarda o marcador de início (sem timeout — espera o botão)
    ser.timeout = None
    while True:
        line = ser.readline().decode("utf-8", errors="ignore").strip()
        if line:
            print(f"  [ESP32] {line}")
        if "REC_START" in line:
            break

    print(f"\nRecebendo {TOTAL_BYTES/1024:.0f} KB...")
    print(f"(a 115200bps vai levar ~{TOTAL_BYTES*8/115200/0.8:.0f} segundos)\n")

    ser.timeout = 10  # timeout de 10s por leitura durante recebimento

    received  = 0
    wav_bytes = bytearray()
    t_start   = time.time()

    while received < TOTAL_BYTES:
        to_read = min(512, TOTAL_BYTES - received)
        chunk   = ser.read(to_read)

        if not chunk:
            print(f"\nERRO: timeout sem dados (recebido {received}/{TOTAL_BYTES} bytes)")
            break

        wav_bytes.extend(chunk)
        received += len(chunk)

        elapsed   = time.time() - t_start
        speed_kbs = (received / 1024) / elapsed if elapsed > 0 else 0
        progress_bar(received, TOTAL_BYTES, speed_kbs)

    elapsed = time.time() - t_start
    print(f"\n\nTransferência concluída em {elapsed:.1f}s")

    # Consome o ##REC_END## e qualquer mensagem restante
    ser.timeout = 2
    while True:
        line = ser.readline().decode("utf-8", errors="ignore").strip()
        if not line:
            break
        print(f"  [ESP32] {line}")

    ser.close()

    # Verifica integridade mínima
    if len(wav_bytes) < WAV_HEADER:
        print(f"ERRO: arquivo muito pequeno ({len(wav_bytes)} bytes), algo deu errado.")
        sys.exit(1)

    # Verifica magic RIFF
    if wav_bytes[:4] != b'RIFF':
        print("AVISO: header RIFF não encontrado — o arquivo pode estar corrompido.")
        print(f"  Primeiros 4 bytes: {wav_bytes[:4]}")

    with open(out_path, "wb") as f:
        f.write(wav_bytes)

    size_kb = os.path.getsize(out_path) / 1024
    print(f"\nArquivo salvo: {out_path}  ({size_kb:.0f} KB)")
    print("\nPróximos passos:")
    print("  1. Abra no Audacity (ou qualquer editor de áudio)")
    print("  2. Selecione cada palavra e exporte como WAV separado")
    print("  3. Suba no Edge Impulse → Data acquisition → Upload data")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--out",  default="gravacao.wav")
    args = parser.parse_args()
    receive(args.port, args.out)
