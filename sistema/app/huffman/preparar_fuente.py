#!/usr/bin/env python3
"""Aplica la primera integracion DSM al Source.cpp original sin sobrescribirlo."""

from pathlib import Path
import re
import sys

carpeta = Path(__file__).resolve().parent
origen = Path(sys.argv[1]) if len(sys.argv) > 1 else carpeta / "Source.cpp"
salida = carpeta / "Source_dsm.cpp"

if not origen.is_file():
    sys.exit(f"No se encontro {origen}. Descargue Source.cpp del repositorio original.")

codigo = origen.read_text(encoding="utf-8-sig")
if "#include \"dsm_buffers.h\"" in codigo:
    sys.exit("El archivo de entrada ya parece adaptado. Use el Source.cpp original.")

patron_buffers = re.compile(
    r"(?m)^(?P<sangria>[ \t]*)char\* inputBuffer = new char\[INPUT_BUFFER_SIZE\];\s*\n"
    r"[ \t]*char\* outputBuffer = new char\[OUTPUT_BUFFER_SIZE\];"
)

reemplazo = (
    "\\g<sangria>BufferDsm reservaEntrada = reservarBufferDsm(static_cast<std::size_t>(INPUT_BUFFER_SIZE));\n"
    "\\g<sangria>BufferDsm reservaSalida = reservarBufferDsm(static_cast<std::size_t>(OUTPUT_BUFFER_SIZE));\n"
    "\\g<sangria>if (!reservaEntrada || !reservaSalida) {\n"
    "\\g<sangria>    cerr << \"No hay memoria disponible en el DSM para los buffers\" << endl;\n"
    "\\g<sangria>    return;\n"
    "\\g<sangria>}\n"
    "\\g<sangria>char* inputBuffer = reservaEntrada.get();\n"
    "\\g<sangria>char* outputBuffer = reservaSalida.get();"
)
codigo, encontrados = patron_buffers.subn(reemplazo, codigo)
if encontrados != 2:
    sys.exit(f"No se modifico el archivo: se esperaban 2 pares de buffers y se encontraron {encontrados}.")

codigo, cant_main = re.subn(
    r"int main\(int argc, char\* argv\[\]\)\s*\{",
    "int main(int argc, char* argv[])\n{\n"
    "    SesionDsm dsm(\"sistema/sistema.conf\");\n"
    "    if (!dsm.iniciada()) {\n"
    "        cerr << \"No se pudo iniciar el DSM; revise sistema/sistema.conf\" << endl;\n"
    "        return 1;\n"
    "    }",
    codigo,
    count=1,
)
if cant_main != 1:
    sys.exit("No se encontro la funcion main original. No se genero el archivo.")

codigo, cant_delete_entrada = re.subn(r"(?m)^[ \t]*delete\[\] inputBuffer;[ \t]*\n", "", codigo)
codigo, cant_delete_salida = re.subn(r"(?m)^[ \t]*delete\[\] outputBuffer;[ \t]*\n", "", codigo)
if cant_delete_entrada != 1 or cant_delete_salida != 1:
    sys.exit("La liberacion de buffers no coincide con la version revisada. No se genero el archivo.")

codigo = '#include "dsm_buffers.h"\n' + codigo
salida.write_text(codigo, encoding="utf-8")
print(f"Fuente adaptada: {salida}")
print("Cambios: inicializacion DSM, dos pares de buffers y liberacion automatica.")
