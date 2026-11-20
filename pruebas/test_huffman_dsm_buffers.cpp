#include "dsm_buffers.h"

#include <cstring>
#include <iostream>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Uso: test_huffman_dsm_buffers <config>\n";
        return 1;
    }

    SesionDsm dsm(argv[1]);
    if (!dsm.iniciada()) {
        std::cerr << "No se pudo iniciar el DSM\n";
        return 1;
    }

    {
        BufferDsm entrada = reservarBufferDsm(64 * 1024);
        BufferDsm salida = reservarBufferDsm(64 * 1024);
        if (!entrada || !salida) {
            std::cerr << "No se pudieron reservar los buffers\n";
            return 1;
        }
        std::memset(entrada.get(), 'A', 64 * 1024);
        std::memcpy(salida.get(), entrada.get(), 64 * 1024);
        if (std::memcmp(entrada.get(), salida.get(), 64 * 1024) != 0) {
            std::cerr << "Los datos no coinciden\n";
            return 1;
        }
        stats_t stats{};
        const int id = sistema_node_id();
        if (id < 0 || sistema_stats(id, &stats) != OK || stats.mem_usada < 128 * 1024) {
            std::cerr << "La reserva no aparece en las estadisticas\n";
            return 1;
        }
        std::cout << "Buffers DSM correctos; memoria usada: " << stats.mem_usada << " bytes\n";
    }

    stats_t final{};
    if (sistema_stats(sistema_node_id(), &final) != OK || final.mem_usada != 0) {
        std::cerr << "Los buffers no se liberaron correctamente\n";
        return 1;
    }
    std::cout << "Liberacion DSM correcta\n";
    return 0;
}
