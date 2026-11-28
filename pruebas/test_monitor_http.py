#!/usr/bin/env python3
"""Comprueba el monitor HTTP real sin depender de paquetes externos."""
import os
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent

with tempfile.TemporaryDirectory(prefix="dsm_http_") as tmp:
    ruta = Path(tmp) / "nodo.conf"
    ruta.write_text(
        "nodoId=0\nipLocal=127.0.0.1\nnodos=0:127.0.0.1\n"
        "puertoBase=28090\npuertoNodos=28091\n"
        "puertoHeartbeat=28092\npuertoPages=28093\npuertoConsulta=28094\n"
        f"logArchivo={tmp}/monitor.log\n"
    )
    proc = subprocess.Popen(
        [str(RAIZ / "build/dsm_nodo"), str(ruta)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, cwd=RAIZ, text=True,
    )
    try:
        pagina = None
        for _ in range(35):
            if proc.poll() is not None:
                break
            try:
                with urllib.request.urlopen("http://127.0.0.1:28095", timeout=1) as r:
                    pagina = r.read().decode("utf-8")
                break
            except (urllib.error.URLError, OSError, TimeoutError):
                time.sleep(0.15)
        if pagina is None:
            raise AssertionError("No se pudo consultar el monitor HTTP")
        for texto in ("DSM - Estado del cluster", "Resumen global",
                      "Tablas de paginas por nodo", "Nodo 0",
                      "MODIFIED", "Pagina", "Proceso", "4096"):
            assert texto in pagina, f"Falta en HTTP: {texto}"
        print("Monitor HTTP: memoria, estadisticas y tabla de paginas correctas")
    finally:
        if proc.poll() is None:
            try:
                proc.communicate(input="salir\n", timeout=8)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try:
                    proc.communicate(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.communicate()
        else:
            salida = proc.stdout.read() if proc.stdout else ""
            if proc.returncode:
                raise AssertionError(f"dsm_nodo termino con {proc.returncode}: {salida[-2000:]}")
