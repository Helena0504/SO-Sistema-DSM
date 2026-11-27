#define _POSIX_C_SOURCE 200809L
extern "C" {
#include "../../api/api.h"
#include "../../global/config.h"
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int huffman_run(int argc, char *argv[]); // Funciones de la aplicacion original adaptada

namespace {
constexpr uint32_t kPages = 128;    // Medio MiB para transferir una muestra
constexpr uint32_t kMagic = 0x48554632; // HUF2
constexpr uint32_t kVersion = 1;

struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t compressed_bytes;
    uint32_t codes_bytes;
    uint64_t original_bytes;
    uint64_t original_checksum;
};

std::atomic<bool> activo{true};

uint64_t checksum(const std::vector<unsigned char> &v)
{
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char b : v) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    return h;
}

bool leer_archivo(const std::string &ruta, std::vector<unsigned char> &datos)
{
    std::ifstream f(ruta, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamoff tam = f.tellg();
    if (tam < 0 || tam > 200000000) return false;
    datos.resize(static_cast<size_t>(tam));
    f.seekg(0);
    return datos.empty() || bool(f.read(reinterpret_cast<char *>(datos.data()), tam));
}

bool escribir_archivo(const std::string &ruta, const unsigned char *datos,
                     size_t tam)
{
    std::ofstream f(ruta, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (tam != 0) f.write(reinterpret_cast<const char *>(datos),
                           static_cast<std::streamsize>(tam));
    return bool(f);
}

std::string ruta_codigos(const std::string &archivo)
{
    const size_t barra = archivo.find_last_of("/\\");
    const std::string dir = barra == std::string::npos ? "" : archivo.substr(0, barra + 1);
    const std::string name = barra == std::string::npos ? archivo : archivo.substr(barra + 1);
    const size_t punto = name.find_last_of('.');
    return dir + (punto == std::string::npos ? name : name.substr(0, punto)) + "_codes.cod";
}

int ejecutar_huffman(const std::string &modo, const std::string &ruta)
{
    std::string exec = "huffman_dsm", flag = "-b", tam = "64";
    char *argv[] = {const_cast<char *>(exec.c_str()), const_cast<char *>(flag.c_str()),
                    const_cast<char *>(tam.c_str()), const_cast<char *>(modo.c_str()),
                    const_cast<char *>(ruta.c_str())};
    return huffman_run(5, argv);
}

void mantener_cluster(config_t config)
{
    while (activo) {
        for (uint32_t i = 0; i < config.cantidad_nodos; ++i) {
            const config_nodo_t &n = config.nodos[i];
            if (n.id == config.nodo_id) continue;
            node_info_t actual{};
            if (sistema_node_info(static_cast<int>(n.id), &actual) != OK || !actual.estado) {
                (void)sistema_red_unirse(n.ip, config.puertoNodos);
            } else {
                (void)sistema_red_heartbeat(n.id);
            }
        }
        uint32_t caidos = 0;
        (void)sistema_red_verificar_nodos(&caidos);
        std::this_thread::sleep_for(std::chrono::milliseconds(config.pausaH));
    }
}

bool esperar_nodo(uint32_t id)
{
    for (int i = 0; i < 120; ++i) {
        node_info_t info{};
        if (sistema_node_info(static_cast<int>(id), &info) == OK && info.estado)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

void mostrar_stats(uint32_t local)
{
    stats_t st{};
    if (sistema_stats(static_cast<int>(local), &st) == OK) {
        std::cout << "[DSM] Paginas enviadas=" << st.paginas_enviadas
                  << ", recibidas=" << st.paginas_recibidas
                  << ", bytes enviados=" << st.bytes_enviados
                  << ", bytes recibidos=" << st.bytes_recibidos << "\n";
    }
}

int productor(unsigned char *area, uint32_t pagina,
              const std::string &entrada)
{
    std::vector<unsigned char> original, compressed, codes;
    if (!leer_archivo(entrada, original) || original.empty()) {
        std::cerr << "No se pudo leer el archivo original o esta vacio: " << entrada << "\n";
        return 1;
    }
    if (ejecutar_huffman("-c", entrada) != 0) {
        std::cerr << "Fallo la compresion Huffman.\n";
        return 1;
    }
    if (!leer_archivo(entrada + ".ece2103", compressed) ||
        !leer_archivo(ruta_codigos(entrada), codes)) {
        std::cerr << "No se encontraron los archivos .ece2103 y _codes.cod.\n";
        return 1;
    }
    const uint64_t tope = static_cast<uint64_t>(kPages - 1) * pagina;
    if (compressed.size() + codes.size() > tope ||
        compressed.size() > UINT32_MAX || codes.size() > UINT32_MAX) {
        std::cerr << "La muestra es demasiado grande para el area DSM de esta prueba.\n";
        return 1;
    }
    Header hdr{};
    hdr.magic = kMagic;
    hdr.version = kVersion;
    hdr.compressed_bytes = static_cast<uint32_t>(compressed.size());
    hdr.codes_bytes = static_cast<uint32_t>(codes.size());
    hdr.original_bytes = original.size();
    hdr.original_checksum = checksum(original);
    std::memcpy(area, &hdr, sizeof(hdr));
    std::memcpy(area + pagina, compressed.data(), compressed.size());
    std::memcpy(area + pagina + compressed.size(), codes.data(), codes.size());
    std::cout << "[Nodo origen] Huffman comprimio " << original.size()
              << " bytes; archivo comprimido " << compressed.size()
              << " bytes; arbol " << codes.size() << " bytes.\n";
    std::cout << "[Nodo origen] Datos preparados en paginas DSM.\n";
    return 0;
}

int consumidor(unsigned char *area, uint32_t page_size,
              uint32_t origen)
{
    if (!esperar_nodo(origen)) {
        std::cerr << "No se pudo conectar con el nodo origen.\n";
        return 1;
    }
    /* La region ya esta marcada como remota. Al leer el encabezado,
       SIGSEGV inicia la recuperacion sin solicitar la pagina manualmente. */
    stats_t antes{};
    if (sistema_stats(static_cast<int>(sistema_node_id()), &antes) != OK) {
        std::cerr << "No se pudieron consultar las estadisticas iniciales.\n";
        return 1;
    }
    Header hdr{};
    std::memcpy(&hdr, area, sizeof(hdr));
    if (hdr.magic != kMagic || hdr.version != kVersion ||
        hdr.compressed_bytes == 0 || hdr.codes_bytes == 0 ||
        static_cast<uint64_t>(hdr.compressed_bytes) + hdr.codes_bytes >
            static_cast<uint64_t>(kPages - 1) * page_size) {
        std::cerr << "Los metadatos recibidos no son validos.\n";
        return 1;
    }
    const uint64_t total = static_cast<uint64_t>(hdr.compressed_bytes) + hdr.codes_bytes;
    const uint32_t paginas = static_cast<uint32_t>((total + page_size - 1) / page_size);
    /* escribir_archivo lee directamente la memoria DSM; los accesos a
       paginas INVALID desencadenan las solicitudes de red automaticamente. */
    const std::string nombre = "build/huffman_recibido.txt";
    if (!escribir_archivo(nombre + ".ece2103", area + page_size, hdr.compressed_bytes) ||
        !escribir_archivo("build/huffman_recibido_codes.cod",
                         area + page_size + hdr.compressed_bytes, hdr.codes_bytes)) {
        std::cerr << "No se pudieron escribir los archivos recuperados.\n";
        return 1;
    }
    stats_t despues{};
    if (sistema_stats(static_cast<int>(sistema_node_id()), &despues) != OK ||
        despues.paginas_recibidas < antes.paginas_recibidas + paginas + 1) {
        std::cerr << "No se recibieron las paginas DSM esperadas.\n";
        return 1;
    }
    std::cout << "[Nodo receptor] " <<
        (despues.paginas_recibidas - antes.paginas_recibidas)
        << " paginas recuperadas automaticamente. Descomprimiendo...\n";
    if (ejecutar_huffman("-d", nombre + ".ece2103") != 0) {
        std::cerr << "Fallo el decodificador Huffman.\n";
        return 1;
    }
    std::vector<unsigned char> resultado;
    if (!leer_archivo(nombre, resultado) ||
        resultado.size() != hdr.original_bytes || checksum(resultado) != hdr.original_checksum) {
        std::cerr << "La comparacion con los datos originales fallo.\n";
        return 1;
    }
    std::cout << "[Nodo receptor] Archivo recuperado: " << nombre
              << " (" << resultado.size() << " bytes).\n";
    std::cout << "[Nodo receptor] Verificacion de contenido correcta.\n";
    return 0;
}
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "Uso: " << argv[0]
                  << " sistema/sistema.conf archivo_original.txt\n";
        return 1;
    }
    config_t conf{};
    if (config_cargar(argv[1], &conf) != 0 || conf.cantidad_nodos < 2) {
        std::cerr << "Configuracion invalida. Se necesitan al menos dos nodos en nodos=.\n";
        return 1;
    }
    const uint32_t origen = conf.nodos[0].id;
    if (conf.pageSize < sizeof(Header) || conf.memoriaNodo <
        static_cast<uint64_t>(kPages) * conf.pageSize + 2 * 64 * 1024) {
        std::cerr << "La configuracion no deja memoria suficiente para Huffman y el area DSM.\n";
        return 1;
    }
    if (sistema_init(argv[1]) != OK) {
        std::cerr << "No se pudo iniciar la API DSM.\n";
        return 1;
    }
    void *memoria = sistema_alloc(static_cast<size_t>(kPages) * conf.pageSize);
    if (memoria == nullptr) {
        std::cerr << "No se pudo reservar la region compartida.\n";
        (void)sistema_finalize();
        return 1;
    }
    auto *area = static_cast<unsigned char *>(memoria);
    int resultado = 0;
    if (conf.nodo_id != origen) {
        /* Todas las paginas de esta reserva se recuperan bajo demanda.
           La API conserva sus IDs logicos para PAGE_REQUEST. */
        if (sistema_region_remota(area, origen) != OK) {
            std::cerr << "No se pudo preparar la region remota de Huffman.\n";
            resultado = 1;
        }
    } else {
        resultado = productor(area, conf.pageSize, argv[2]);
    }
    if (resultado == 0) {
        if (sistema_red_iniciar() != OK) {
            std::cerr << "No se pudo abrir la comunicacion TCP. ¿Ya hay otro dsm_nodo en ejecucion?\n";
            resultado = 1;
        } else {
            std::thread mantenimiento(mantener_cluster, conf);
            std::cout << "[DSM] Nodo " << conf.nodo_id << " en " << conf.ip_local
                      << " (origen " << origen << ").\n";
            if (conf.nodo_id != origen) {
                resultado = consumidor(area, conf.pageSize, origen);
            } else {
                std::cout << "[Nodo origen] Esperando que los nodos reciban los datos.\n";
                std::cout << "[Nodo origen] Presione ENTER cuando hayan terminado.\n";
                (void)std::getchar();
            }
            mostrar_stats(conf.nodo_id);
            activo = false;
            mantenimiento.join();
        }
    }
    (void)sistema_finalize();
    return resultado;
}
