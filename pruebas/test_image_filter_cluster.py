#!/usr/bin/env python3
"""Ejecuta Image Filter Engine con dos nodos y verifica los resultados DSM."""
from pathlib import Path
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build/app1'


def crear_png(ruta):
    w, h = 40, 30
    def trozo(tipo, datos):
        return struct.pack('!I', len(datos)) + tipo + datos + struct.pack('!I', zlib.crc32(tipo + datos) & 0xffffffff)
    scan = b''.join(b'\x00' + bytes((10, 80, 150)) * w for _ in range(h))
    ruta.write_bytes(b'\x89PNG\r\n\x1a\n' + trozo(b'IHDR', struct.pack('!IIBBBBB', w, h, 8, 2, 0, 0, 0)) + trozo(b'IDAT', zlib.compress(scan)) + trozo(b'IEND', b''))


def esperar_puerto(host, port, proceso, max_segundos=12):
    fin = time.monotonic() + max_segundos
    while time.monotonic() < fin:
        if proceso.poll() is not None:
            raise RuntimeError('El nodo coordinador termino prematuramente')
        try:
            with socket.create_connection((host, port), timeout=.2):
                return
        except OSError:
            time.sleep(.15)
    raise RuntimeError('El nodo coordinador no abrio el puerto TCP')


def verificar_png(ruta, esperado):
    datos = ruta.read_bytes()
    if not datos.startswith(b'\x89PNG\r\n\x1a\n'):
        raise RuntimeError(f'No es PNG valido: {ruta}')
    pos = 8
    componentes = []
    while pos < len(datos):
        n = struct.unpack_from('!I', datos, pos)[0]
        tipo = datos[pos + 4:pos + 8]
        valor = datos[pos + 8:pos + 8 + n]
        pos += 12 + n
        if tipo == b'IDAT':
            componentes.append(valor)
    decoded = zlib.decompress(b''.join(componentes))
    if len(decoded) != 30 * (1 + 40 * 3):
        raise RuntimeError(f'Tamano PNG incorrecto: {ruta}')
    anterior = bytearray(120)
    for y in range(30):
        linea = decoded[y * 121:(y + 1) * 121]
        filtro = linea[0]
        pixeles = bytearray(linea[1:])
        for x in range(120):
            izquierda = pixeles[x - 3] if x >= 3 else 0
            arriba = anterior[x]
            diagonal = anterior[x - 3] if x >= 3 else 0
            if filtro == 0:
                correccion = 0
            elif filtro == 1:
                correccion = izquierda
            elif filtro == 2:
                correccion = arriba
            elif filtro == 3:
                correccion = (izquierda + arriba) // 2
            elif filtro == 4:
                p = izquierda + arriba - diagonal
                distancias = (abs(p - izquierda), abs(p - arriba), abs(p - diagonal))
                correccion = (izquierda, arriba, diagonal)[distancias.index(min(distancias))]
            else:
                raise RuntimeError(f'Filtro PNG inesperado: {filtro}')
            pixeles[x] = (pixeles[x] + correccion) & 255
        if pixeles != bytes((esperado,)) * 120:
            raise RuntimeError(f'Pixeles incorrectos en {ruta}, fila {y}: {pixeles[:3]}, esperado {esperado}')
        anterior = pixeles


def main():
    if not EXE.is_file():
        raise RuntimeError('Compile build/app1 primero')
    with tempfile.TemporaryDirectory(prefix='dsm-app1-') as td:
        tmp = Path(td)
        trabajo = tmp / 'app1'
        (trabajo / 'samples/input').mkdir(parents=True)
        (trabajo / 'samples/output').mkdir(parents=True)
        crear_png(trabajo / 'samples/input/entrada.png')
        conf = tmp / 'sistema.conf'
        conf.write_text('nodos=0:127.0.0.2,1:127.0.0.3\npageSize=4096\nmemoriaNodo=8388608\npausaH=1000\nmuertoH=3500\n' + f'logArchivo={tmp}/dsm.log\n')
        args = [str(EXE), str(conf), '--filtro', '1', '--imagenes', '2', '--hilos', '2', '--sin-serial', '--monitor', '0', '--esperar', '15', '--dir', str(trabajo)]
        origen_log = tmp / 'coordinador.log'
        trabajador_log = tmp / 'trabajador.log'
        coordinador = trabajador = None
        try:
            with origen_log.open('w') as lf, trabajador_log.open('w') as wf:
                coordinador = subprocess.Popen(args, cwd=ROOT, stdout=lf, stderr=subprocess.STDOUT, env=dict(os.environ, DSM_NODO_ID='0'))
                esperar_puerto('127.0.0.2', 28001, coordinador)
                trabajador = subprocess.Popen(args, cwd=ROOT, stdout=wf, stderr=subprocess.STDOUT, env=dict(os.environ, DSM_NODO_ID='1'))
                trabajador.wait(timeout=40)
                coordinador.wait(timeout=40)
            logs = origen_log.read_text(errors='replace')
            log_trabajador = trabajador_log.read_text(errors='replace')
            if coordinador.returncode or trabajador.returncode:
                raise RuntimeError(f'Error en ejecucion: coordinador={coordinador.returncode}, trabajador={trabajador.returncode}')
            if '[BUZON] Resumen del nodo 1 recuperado mediante memoria DSM.' not in logs or '(sin resultado)' in logs:
                raise RuntimeError('El coordinador no recupero el resumen remoto')
            # Comprobar que hubo transferencia real desde el nodo trabajador,
            # no solamente un mensaje de exito del programa.
            paginas_recibidas = [int(n) for n in re.findall(r'\| Red / pager\s*:[^\n]*recibidas=(\d+)', logs)]
            if not paginas_recibidas or max(paginas_recibidas) < 1:
                raise RuntimeError('El coordinador no registro paginas DSM recibidas por red')
            # Grayscale usa 0.299R+0.587G+0.114B y cada nodo usa un brillo distinto.
            verificar_png(trabajo / 'samples/output/app1_nodo0_Grayscale_img0.png', int(.299 * 10 + .587 * 80 + .114 * 150))
            verificar_png(trabajo / 'samples/output/app1_nodo1_Grayscale_img1.png', int(.299 * 39 + .587 * 109 + .114 * 179))
            print('[Image Filter] 2 nodos: paginas DSM, resumen remoto y PNG verificados')
            print('Image Filter Engine con paginacion automatica: prueba correcta')
        except Exception:
            print('--- Coordinador ---', file=sys.stderr)
            print(origen_log.read_text(errors='replace'), file=sys.stderr)
            print('--- Trabajador ---', file=sys.stderr)
            print(trabajador_log.read_text(errors='replace'), file=sys.stderr)
            raise
        finally:
            for p in (coordinador, trabajador):
                if p is not None and p.poll() is None:
                    p.kill()
                    p.wait()

if __name__ == '__main__':
    main()
