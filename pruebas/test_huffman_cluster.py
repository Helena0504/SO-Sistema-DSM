#!/usr/bin/env python3
"""Comprueba Huffman con dos procesos DSM y paginas recuperadas por SIGSEGV."""
from pathlib import Path
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
EXE = BUILD / "huffman_cluster"
INPUT = BUILD / "huffman_cluster_entrada.txt"
OUTPUT = BUILD / "huffman_recibido.txt"
ORIGINAL = ROOT / "pruebas/datos/huffman_demo.txt"


def esperar_puerto(host, port, proceso, max_segundos=12):
    fin = time.monotonic() + max_segundos
    while time.monotonic() < fin:
        if proceso.poll() is not None:
            raise RuntimeError("El nodo origen termino antes de abrir el servidor TCP")
        try:
            with socket.create_connection((host, port), timeout=0.3):
                return
        except OSError:
            time.sleep(0.15)
    raise RuntimeError("El nodo origen no abrio el servidor TCP")


def main():
    if not EXE.is_file():
        raise RuntimeError("Primero compile build/huffman_cluster")
    BUILD.mkdir(exist_ok=True)
    shutil.copyfile(ORIGINAL, INPUT)
    OUTPUT.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix="dsm-huffman-") as temp:
        carpeta = Path(temp)
        conf = carpeta / "sistema.conf"
        # Cada IP de loopback es independiente, aunque ambas esten en Linux.
        conf.write_text("nodos=0:127.0.0.2,1:127.0.0.3\n"
                        "pageSize=4096\nmemoriaNodo=1048576\n"
                        "pausaH=1000\nmuertoH=3500\n"
                        f"logArchivo={carpeta / 'dsm.log'}\n")
        env0 = dict(os.environ, DSM_NODO_ID="0")
        env1 = dict(os.environ, DSM_NODO_ID="1")
        origen_log = carpeta / "origen.log"
        receptor_log = carpeta / "receptor.log"
        origen = None
        try:
            with origen_log.open("w") as lf:
                origen = subprocess.Popen([str(EXE), str(conf), str(INPUT)],
                    cwd=ROOT, stdin=subprocess.PIPE, stdout=lf,
                    stderr=subprocess.STDOUT, env=env0, text=True)
                esperar_puerto("127.0.0.2", 28001, origen)
                with receptor_log.open("w") as rf:
                    resultado = subprocess.run(
                        [str(EXE), str(conf), "build/archivo_inexistente.txt"],
                        cwd=ROOT, stdout=rf, stderr=subprocess.STDOUT,
                        env=env1, timeout=30)
                assert origen.stdin is not None
                origen.stdin.write("\n")
                origen.stdin.flush()
                origen.wait(timeout=15)
            texto_receptor = receptor_log.read_text(errors="replace")
            if (resultado.returncode != 0 or origen.returncode != 0 or
                not OUTPUT.exists() or OUTPUT.read_bytes() != INPUT.read_bytes() or
                "paginas recuperadas automaticamente" not in texto_receptor or
                "Verificacion de contenido correcta" not in texto_receptor):
                raise RuntimeError("Fallo la comprobacion de Huffman entre nodos")
            for linea in texto_receptor.splitlines():
                if "paginas recuperadas automaticamente" in linea or "Verificacion de contenido correcta" in linea:
                    print(linea)
            print("Huffman con paginacion remota automatica: prueba correcta")
        except (Exception, KeyboardInterrupt):
            if origen is not None and origen.poll() is None:
                origen.terminate()
                try: origen.wait(timeout=3)
                except subprocess.TimeoutExpired: origen.kill(); origen.wait()
            print("----- Nodo origen -----", file=sys.stderr)
            print(origen_log.read_text(errors="replace"), file=sys.stderr)
            print("----- Nodo receptor -----", file=sys.stderr)
            if receptor_log.exists():
                print(receptor_log.read_text(errors="replace"), file=sys.stderr)
            raise
        finally:
            if origen is not None and origen.poll() is None:
                origen.kill()
                origen.wait()

if __name__ == "__main__":
    main()
