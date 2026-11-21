#!/usr/bin/env python3
"""Convierte Source_dsm.cpp en una funcion para integrarla al proceso del nodo.

El script no modifica los fuentes originales. Source_dsm.cpp debe haberse
creado antes con preparar_fuente.py.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parent
src = root / "Source_dsm.cpp"
dst = root / "Source_cluster.cpp"
if not src.exists():
    raise SystemExit("Falta Source_dsm.cpp. Ejecute primero preparar_fuente.py")
text = src.read_text(encoding="utf-8")
old = ('    SesionDsm dsm("sistema/sistema.conf");\n'
       '    if (!dsm.iniciada()) {\n'
       '        cerr << "No se pudo iniciar el DSM; revise sistema/sistema.conf" << endl;\n'
       '        return 1;\n'
       '    }')
if text.count(old) != 1:
    raise SystemExit("No se reconoce la inicializacion del DSM en Source_dsm.cpp.")
text = text.replace(old, '', 1)
text, n = re.subn(r'\bint main\(int argc, char\* argv\[\]\)',
                  'int huffman_run(int argc, char* argv[])', text, count=1)
if n != 1:
    raise SystemExit("No se encontro main en Source_dsm.cpp")
dst.write_text(text, encoding='utf-8')
makefile = root.parents[2] / 'Makefile'
include = '-include sistema/app/huffman/huffman_cluster.mk'
if not makefile.exists():
    raise SystemExit('No se encontro el Makefile de la raiz del proyecto.')
mk = makefile.read_text(encoding='utf-8')
if include not in mk:
    makefile.write_text(mk.rstrip() + '\n\n' + include + '\n', encoding='utf-8')
print('Generado: sistema/app/huffman/Source_cluster.cpp')
print('Makefile preparado para compilar build/huffman_cluster')
