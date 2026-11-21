// ============================================================
// main.cpp  -  app1: Image Filter Engine sobre el sistema DSM
//
// Adaptacion del motor de filtros de imagenes (Grayscale, Gaussian,
// Sharpen, Sobel, Emboss) para que:
//   1. cargue y procese MUCHAS imagenes,
//   2. guarde todos los buffers (entrada y resultado) en memoria DSM
//      con sistema_alloc(), no con malloc()/stbi_load(),
//   3. muestre como se comporta la memoria del sistema (paginas MSI,
//      uso por nodo, RSS real, contadores de red),
//   4. reparta las imagenes entre los nodos del cluster y recoja los
//      resultados mediante paginas DSM (PAGE_REQUEST / INVALIDATE).
//
// Todos los nodos ejecutan el mismo programa con el mismo sistema.conf:
//   ./build/app1 sistema/sistema.conf [opciones]
// ============================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <omp.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../include/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../include/stb_image_write.h"

// El DSM esta escrito en C.
extern "C"
{
#include "../../../api/api.h"
#include "../../../global/config.h"
}

using namespace std;
namespace fs = std::filesystem;

static const int CH = 3; // todas las imagenes se trabajan como RGB

// ------------------------------------------------------------
// Estado global y utilidades
// ------------------------------------------------------------

static volatile sig_atomic_t g_continuar = 1;
static void terminar(int) { g_continuar = 0; }

enum Fase
{
    FASE_INICIO = 0,
    FASE_CARGA,
    FASE_PROCESO,
    FASE_REPORTE,
    FASE_PUBLICAR,
    FASE_ESPERA
};
static const char *NOMBRE_FASE[] = {"inicio", "carga", "proceso", "reporte", "publicar", "espera"};
static atomic<int> g_fase{FASE_INICIO};
static atomic<uint64_t> g_imagenes{0};  // imagenes cargadas en DSM
static atomic<uint64_t> g_trabajo{0};   // aplicaciones de filtro terminadas
static uint64_t g_trabajo_total = 0;
static atomic<bool> g_monitor_activo{true};
static int g_monitor_seg = 2;
static int g_nodo = 0;

static double ahora()
{
    return chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count();
}

static string mib(uint64_t bytes)
{
    char b[32];
    if (bytes < 1048576)
        snprintf(b, sizeof(b), "%.1f KiB", (double)bytes / 1024.0);
    else
        snprintf(b, sizeof(b), "%.1f MiB", (double)bytes / 1048576.0);
    return b;
}

static uint64_t rss_bytes()
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return 0;
    char linea[256];
    uint64_t kb = 0;
    while (fgets(linea, sizeof(linea), f))
        if (sscanf(linea, "VmRSS: %lu kB", (unsigned long *)&kb) == 1)
            break;
    fclose(f);
    return kb * 1024;
}

static void dormir_ms(long ms)
{
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

// ------------------------------------------------------------
// Opciones de linea de comandos
// ------------------------------------------------------------

struct Opciones
{
    string conf;
    int filtro = 0;      // 1..5 un filtro, 6 todos, 0 = preguntar
    long imagenes = 0;   // total en el cluster; 0 = llenar la memoria
    int escala = 1;      // agranda cada imagen (vecino mas cercano)
    double llenar = 70;  // % de memoriaNodo a ocupar
    int pasadas = 1;     // repite el lote completo
    int hilos = 0;       // 0 = omp_get_max_threads()
    string dir;          // carpeta de app1
    bool guardarTodas = false;
    bool serial = true;  // medir serial vs paralelo
    int esperar = 60;    // segundos esperando que se unan los nodos
};

static void uso(const char *prog)
{
    fprintf(stderr,
            "Uso: %s sistema/sistema.conf [opciones]\n"
            "  --filtro N       1 Grayscale, 2 Gaussian, 3 Sharpen, 4 Sobel, 5 Emboss, 6 todos\n"
            "  --imagenes N     total de imagenes en TODO el cluster (default: llenar la memoria)\n"
            "  --llenar PCT     porcentaje de memoriaNodo a ocupar (default 70)\n"
            "  --escala K       agranda cada imagen K veces por lado (default 1)\n"
            "  --pasadas P      repite el lote P veces (default 1)\n"
            "  --hilos T        hilos OpenMP (default: todos los nucleos)\n"
            "  --dir RUTA       carpeta app1 (default: sistema/app/app1)\n"
            "  --guardar-todas  guarda todos los resultados (default: solo el primero)\n"
            "  --sin-serial     omite la medicion serial vs paralelo\n"
            "  --monitor SEG    periodo del monitor de memoria (default 2, 0 = apagado)\n"
            "  --esperar SEG    espera maxima a que se conecten los nodos (default 60)\n",
            prog);
}

static bool parsear(int argc, char **argv, Opciones &o)
{
    if (argc < 2)
        return false;
    o.conf = argv[1];
    for (int i = 2; i < argc; i++)
    {
        string a = argv[i];
        auto sig = [&](const char *nombre) -> const char *
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "Falta el valor de %s\n", nombre);
                return NULL;
            }
            return argv[++i];
        };
        const char *v = NULL;
        if (a == "--filtro" && (v = sig("--filtro")))
            o.filtro = atoi(v);
        else if (a == "--imagenes" && (v = sig("--imagenes")))
            o.imagenes = atol(v);
        else if (a == "--llenar" && (v = sig("--llenar")))
            o.llenar = atof(v);
        else if (a == "--escala" && (v = sig("--escala")))
            o.escala = max(1, atoi(v));
        else if (a == "--pasadas" && (v = sig("--pasadas")))
            o.pasadas = max(1, atoi(v));
        else if (a == "--hilos" && (v = sig("--hilos")))
            o.hilos = max(1, atoi(v));
        else if (a == "--dir" && (v = sig("--dir")))
            o.dir = v;
        else if (a == "--monitor" && (v = sig("--monitor")))
            g_monitor_seg = max(0, atoi(v));
        else if (a == "--esperar" && (v = sig("--esperar")))
            o.esperar = max(0, atoi(v));
        else if (a == "--guardar-todas")
            o.guardarTodas = true;
        else if (a == "--sin-serial")
            o.serial = false;
        else
        {
            fprintf(stderr, "Opcion desconocida: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------
// Filtros (misma logica del main original)
// ------------------------------------------------------------

enum
{
    F_GRIS = 1,
    F_BLUR,
    F_SHARPEN,
    F_SOBEL,
    F_EMBOSS
};

static const char *nombre_filtro(int f)
{
    switch (f)
    {
    case F_GRIS:    return "Grayscale";
    case F_BLUR:    return "Gaussian Blur";
    case F_SHARPEN: return "Sharpen";
    case F_SOBEL:   return "Sobel";
    case F_EMBOSS:  return "Emboss";
    case 6:         return "Todos";
    }
    return "?";
}

static const float V = 1.0f / 16.0f;
static const float K_BLUR[3][3] = {{1 * V, 2 * V, 1 * V}, {2 * V, 4 * V, 2 * V}, {1 * V, 2 * V, 1 * V}};
static const float K_SHARPEN[3][3] = {{0, -1, 0}, {-1, 5, -1}, {0, -1, 0}};
static const float K_EMBOSS[3][3] = {{-2, -1, 0}, {-1, 1, 1}, {0, 1, 2}};
static const float K_SOBEL_X[3][3] = {{-1, 0, 1}, {-2, 0, 2}, {-1, 0, 1}};
static const float K_SOBEL_Y[3][3] = {{-1, -2, -1}, {0, 0, 0}, {1, 2, 1}};

static inline int gris_de(const unsigned char *p)
{
    return (int)(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]);
}

static inline unsigned char limitar(int v)
{
    return (unsigned char)std::min(std::max(v, 0), 255);
}

static void filtro_grayscale(const unsigned char *img, unsigned char *result, int w, int h)
{
    long n = (long)w * h;
#pragma omp parallel for schedule(static)
    for (long p = 0; p < n; p++)
    {
        int g = gris_de(img + p * CH);
        result[p * CH] = result[p * CH + 1] = result[p * CH + 2] = (unsigned char)g;
    }
}

// Convolucion 3x3. Con usarGris=true trabaja sobre la luminancia, que es lo
// que hacia el original al convertir la imagen a gris antes de Emboss.
// El borde de 1 pixel queda en 0 (la memoria DSM nace en cero).
static void aplicar_kernel(const unsigned char *img, unsigned char *result, int w, int h,
                           const float K[3][3], bool usarGris)
{
#pragma omp parallel for schedule(static)
    for (int y = 1; y < h - 1; y++)
    {
        for (int x = 1; x < w - 1; x++)
        {
            float r = 0, g = 0, b = 0;
            for (int ky = -1; ky <= 1; ky++)
            {
                for (int kx = -1; kx <= 1; kx++)
                {
                    const unsigned char *p = img + (((long)(y + ky) * w) + (x + kx)) * CH;
                    float k = K[ky + 1][kx + 1];
                    if (usarGris)
                        r += gris_de(p) * k;
                    else
                    {
                        r += p[0] * k;
                        g += p[1] * k;
                        b += p[2] * k;
                    }
                }
            }
            if (usarGris)
                g = b = r;
            unsigned char *o = result + (((long)y * w) + x) * CH;
            o[0] = limitar((int)fabsf(r));
            o[1] = limitar((int)fabsf(g));
            o[2] = limitar((int)fabsf(b));
        }
    }
}

static void filtro_sobel(const unsigned char *img, unsigned char *result, int w, int h)
{
#pragma omp parallel for schedule(static)
    for (int y = 1; y < h - 1; y++)
    {
        for (int x = 1; x < w - 1; x++)
        {
            float gx = 0, gy = 0;
            for (int ky = -1; ky <= 1; ky++)
            {
                for (int kx = -1; kx <= 1; kx++)
                {
                    int g = gris_de(img + (((long)(y + ky) * w) + (x + kx)) * CH);
                    gx += g * K_SOBEL_X[ky + 1][kx + 1];
                    gy += g * K_SOBEL_Y[ky + 1][kx + 1];
                }
            }
            unsigned char v = limitar((int)sqrtf(gx * gx + gy * gy));
            unsigned char *o = result + (((long)y * w) + x) * CH;
            o[0] = o[1] = o[2] = v;
        }
    }
}

static void aplicar_filtro(int f, const unsigned char *in, unsigned char *out, int w, int h)
{
    switch (f)
    {
    case F_GRIS:    filtro_grayscale(in, out, w, h); break;
    case F_BLUR:    aplicar_kernel(in, out, w, h, K_BLUR, false); break;
    case F_SHARPEN: aplicar_kernel(in, out, w, h, K_SHARPEN, false); break;
    case F_SOBEL:   filtro_sobel(in, out, w, h); break;
    case F_EMBOSS:  aplicar_kernel(in, out, w, h, K_EMBOSS, true); break;
    }
}

// ------------------------------------------------------------
// Imagenes de origen (heap normal, son pocas) e imagenes en DSM
// ------------------------------------------------------------

struct Fuente
{
    string nombre;
    int w = 0, h = 0;
    vector<unsigned char> px; // RGB
};

struct ImagenDSM
{
    unsigned char *in = NULL;  // memoria DSM
    unsigned char *out = NULL; // memoria DSM
    int w = 0, h = 0;
    size_t bytes = 0;
    uint64_t id_global = 0;
};

static void escalar(Fuente &f, int k)
{
    if (k <= 1)
        return;
    int nw = f.w * k, nh = f.h * k;
    vector<unsigned char> nuevo((size_t)nw * nh * CH);
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++)
            memcpy(&nuevo[((size_t)y * nw + x) * CH], &f.px[((size_t)(y / k) * f.w + (x / k)) * CH], CH);
    f.px.swap(nuevo);
    f.w = nw;
    f.h = nh;
}

static Fuente sintetica(int escala)
{
    Fuente f;
    f.nombre = "sintetica";
    f.w = 640;
    f.h = 360;
    f.px.resize((size_t)f.w * f.h * CH);
    for (int y = 0; y < f.h; y++)
        for (int x = 0; x < f.w; x++)
        {
            unsigned char *p = &f.px[((size_t)y * f.w + x) * CH];
            bool tablero = ((x / 40) + (y / 40)) % 2;
            p[0] = (unsigned char)(x * 255 / f.w);
            p[1] = (unsigned char)(y * 255 / f.h);
            p[2] = tablero ? 220 : 40;
        }
    escalar(f, escala);
    return f;
}

static vector<Fuente> cargar_fuentes(const string &dir_in, int escala)
{
    vector<Fuente> fuentes;
    vector<fs::path> rutas;
    error_code ec;
    for (auto &e : fs::directory_iterator(dir_in, ec))
    {
        if (!e.is_regular_file())
            continue;
        string ext = e.path().extension().string();
        transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga")
            rutas.push_back(e.path());
    }
    sort(rutas.begin(), rutas.end());
    for (auto &r : rutas)
    {
        Fuente f;
        int w, h, c;
        unsigned char *d = stbi_load(r.string().c_str(), &w, &h, &c, CH); // fuerza RGB
        if (!d)
        {
            printf("[CARGA] No se pudo leer %s: %s\n", r.string().c_str(), stbi_failure_reason());
            continue;
        }
        f.nombre = r.filename().string();
        f.w = w;
        f.h = h;
        f.px.assign(d, d + (size_t)w * h * CH);
        stbi_image_free(d);
        escalar(f, escala);
        fuentes.push_back(std::move(f));
    }
    if (fuentes.empty())
    {
        printf("[CARGA] No hay imagenes en %s; se usa una imagen sintetica.\n", dir_in.c_str());
        fuentes.push_back(sintetica(escala));
    }
    return fuentes;
}

// Copia la fuente al buffer DSM; 'variante' cambia el brillo para que cada
// copia sea una imagen distinta (y su resultado tambien).
static void copiar_a_dsm(unsigned char *dst, const Fuente &f, uint64_t variante)
{
    int delta = (int)((variante * 29) % 96);
    size_t n = f.px.size();
    const unsigned char *s = f.px.data();
#pragma omp parallel for schedule(static)
    for (long i = 0; i < (long)n; i++)
        dst[i] = limitar(s[i] + delta);
}

static uint64_t fnv1a(const unsigned char *d, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++)
    {
        h ^= d[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// ------------------------------------------------------------
// Observacion de la memoria DSM
// ------------------------------------------------------------

struct Histograma
{
    uint64_t total = 0, libres = 0, inval = 0, shared = 0, modif = 0;
    string mapa;
};

// Recorre la tabla de paginas del nodo a traves del API.
static Histograma histograma(uint32_t npag, int celdas = 96)
{
    Histograma h;
    h.total = npag;
    uint32_t por_celda = (uint32_t)max<uint32_t>(1, (npag + celdas - 1) / celdas);
    uint32_t cI = 0, cS = 0, cM = 0;
    for (uint32_t i = 0; i < npag; i++)
    {
        page_state_t st;
        if (sistema_page_get_state(i, &st) != OK)
            h.libres++;
        else if (st == PAGE_INVALID)
            h.inval++, cI++;
        else if (st == PAGE_SHARED)
            h.shared++, cS++;
        else
            h.modif++, cM++;
        if ((i + 1) % por_celda == 0 || i + 1 == npag)
        {
            h.mapa += cI ? 'I' : cS ? 'S' : cM ? 'M' : '.';
            cI = cS = cM = 0;
        }
    }
    return h;
}

static void reporte_memoria(const char *titulo, const config_t &cfg, uint32_t npag, bool con_mapa = true)
{
    stats_t s;
    if (sistema_stats((int)cfg.nodo_id, &s) != OK)
    {
        printf("[MEM] no se pudieron leer las estadisticas\n");
        return;
    }
    Histograma h = histograma(npag);
    double pct = s.mem_total ? 100.0 * s.mem_usada / s.mem_total : 0;
    printf("\n+---- MEMORIA DSM | Nodo %u (%s) | %s\n", cfg.nodo_id, cfg.ip_local, titulo);
    printf("| DSM usada    : %s de %s (%.1f%%)   RSS real del proceso: %s\n",
           mib(s.mem_usada).c_str(), mib(s.mem_total).c_str(), pct, mib(rss_bytes()).c_str());
    printf("| Paginas      : %llu total | %llu libres | %llu MODIFIED | %llu SHARED | %llu INVALID\n",
           (unsigned long long)h.total, (unsigned long long)h.libres, (unsigned long long)h.modif,
           (unsigned long long)h.shared, (unsigned long long)h.inval);
    if (con_mapa)
        printf("| Mapa         : [%s]  (. libre, M modificada, S compartida, I invalida)\n", h.mapa.c_str());
    printf("| Red / pager  : fallos=%llu enviadas=%llu recibidas=%llu invalidaciones=%llu "
           "bytes_env=%s bytes_rec=%s\n",
           (unsigned long long)s.faults_pagina, (unsigned long long)s.paginas_enviadas,
           (unsigned long long)s.paginas_recibidas, (unsigned long long)s.invalidadas,
           mib(s.bytes_enviados).c_str(), mib(s.bytes_recibidos).c_str());
    printf("| Operaciones  : alloc=%llu free=%llu errores_red=%llu uptime=%.1fs\n",
           (unsigned long long)s.solicitudes_memoria, (unsigned long long)s.liberaciones_memoria,
           (unsigned long long)s.errores_red, s.uptime_ms / 1000.0);
    printf("+----\n");
    fflush(stdout);
}

static const char *nombre_estado(page_state_t e)
{
    return e == PAGE_INVALID ? "INVALID" : e == PAGE_SHARED ? "SHARED" : "MODIFIED";
}

// Monitor: una linea de memoria cada N segundos mientras se carga o procesa.
static void *hilo_monitor(void *)
{
    while (g_monitor_activo && g_monitor_seg > 0)
    {
        for (int i = 0; i < g_monitor_seg * 10 && g_monitor_activo; i++)
            dormir_ms(100);
        int fase = g_fase;
        if (!g_monitor_activo || (fase != FASE_CARGA && fase != FASE_PROCESO))
            continue;
        stats_t s;
        if (sistema_stats(g_nodo, &s) != OK)
            continue;
        char prog[64];
        if (fase == FASE_CARGA)
            snprintf(prog, sizeof(prog), "imagenes cargadas %llu", (unsigned long long)g_imagenes.load());
        else
            snprintf(prog, sizeof(prog), "filtros aplicados %llu/%llu", (unsigned long long)g_trabajo.load(),
                     (unsigned long long)g_trabajo_total);
        printf("[MON nodo %d | %-7s] DSM %s / %s (%.1f%%) | RSS %s | %s\n", g_nodo,
               NOMBRE_FASE[fase], mib(s.mem_usada).c_str(), mib(s.mem_total).c_str(),
               s.mem_total ? 100.0 * s.mem_usada / s.mem_total : 0.0, mib(rss_bytes()).c_str(), prog);
        fflush(stdout);
    }
    return NULL;
}

// ------------------------------------------------------------
// Cluster: conexion de nodos + heartbeat (igual que app/cluster/main.c)
// ------------------------------------------------------------

static void mantener_cluster(const config_t *conf)
{
    for (uint32_t i = 0; i < conf->cantidad_nodos; i++)
    {
        const config_nodo_t *remoto = &conf->nodos[i];
        if (remoto->id == conf->nodo_id)
            continue;
        node_info_t info;
        if (sistema_node_info((int)remoto->id, &info) != OK || !info.estado)
        {
            if (sistema_red_unirse(remoto->ip, conf->puertoNodos) == OK)
                printf("[RED] Nodo %u conectado: %s\n", remoto->id, remoto->ip);
        }
        else
            (void)sistema_red_heartbeat(remoto->id);
    }
    uint32_t caidos = 0;
    if (sistema_red_verificar_nodos(&caidos) == OK && caidos > 0)
        printf("[RED] %u nodo(s) sin heartbeat.\n", caidos);
}

static atomic<bool> g_cluster_activo{true};
static void *hilo_cluster(void *datos)
{
    const config_t *conf = (const config_t *)datos;
    while (g_cluster_activo)
    {
        mantener_cluster(conf);
        dormir_ms((long)conf->pausaH);
    }
    return NULL;
}

static int nodos_conectados(const config_t &cfg)
{
    int n = 0;
    for (uint32_t i = 0; i < cfg.cantidad_nodos; i++)
    {
        if (cfg.nodos[i].id == cfg.nodo_id)
        {
            n++;
            continue;
        }
        node_info_t info;
        if (sistema_node_info((int)cfg.nodos[i].id, &info) == OK && info.estado)
            n++;
    }
    return n;
}

static bool nodo_vivo(uint32_t id)
{
    node_info_t info;
    return sistema_node_info((int)id, &info) == OK && info.estado;
}

// ------------------------------------------------------------
// Buzon de resultados: una pagina DSM por nodo (pagina i = nodo i de la lista).
// Cada nodo escribe su resumen en su pagina; el coordinador (primer nodo
// de sistema.conf) la trae con PAGE_REQUEST. Asi los resultados viajan
// por el propio protocolo MSI del DSM.
// ------------------------------------------------------------

#define RESUMEN_MAGIC 0x41505031u // "APP1"

struct Resumen
{
    uint32_t magic, nodo, imagenes, pasadas, hilos, filtro;
    uint64_t pixeles;
    uint64_t dsm_total, dsm_usada;
    uint64_t pag_total, pag_libres, pag_m, pag_s, pag_i;
    uint64_t rss, checksum;
    uint64_t faults, enviadas, recibidas, invalidadas, bytes_env, bytes_rec;
    double t_carga, t_proceso, speedup;
    char ip[46];
};
static_assert(sizeof(Resumen) <= 4096, "El resumen debe caber en una pagina");

// ------------------------------------------------------------
// main
// ------------------------------------------------------------

int main(int argc, char **argv)
{
    Opciones op;
    if (!parsear(argc, argv, op))
    {
        uso(argv[0]);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, terminar);
    signal(SIGTERM, terminar);

    // ---- carpeta de trabajo de app1 (samples/input, samples/output) ----
    if (op.dir.empty())
    {
        const char *cand[] = {"sistema/app/app1", "app/app1", "..", "."};
        for (const char *c : cand)
            if (fs::exists(fs::path(c) / "samples" / "input"))
            {
                op.dir = c;
                break;
            }
        if (op.dir.empty())
            op.dir = "sistema/app/app1";
    }
    string dir_in = (fs::path(op.dir) / "samples" / "input").string();
    string dir_out = (fs::path(op.dir) / "samples" / "output").string();
    error_code ec;
    fs::create_directories(dir_out, ec);

    // ---- menu original (solo si no se pasa --filtro) ----
    if (op.filtro < 1 || op.filtro > 6)
    {
        int choice = 0;
        cout << "Select Filter :\n1.Grayscale\n2.Gaussian Blur\n3.Sharpen\n4.Sobel Edge Detection\n5.Emboss\n6.Todos\n";
        if (!(cin >> choice) || choice < 1 || choice > 6)
        {
            cout << "Opcion invalida, se usa Gaussian Blur.\n";
            choice = F_BLUR;
        }
        op.filtro = choice;
    }
    vector<int> filtros;
    if (op.filtro == 6)
        filtros = {F_GRIS, F_BLUR, F_SHARPEN, F_SOBEL, F_EMBOSS};
    else
        filtros = {op.filtro};
    int hilos = op.hilos > 0 ? op.hilos : omp_get_max_threads();

    // ---- DSM ----
    config_t cfg;
    if (config_cargar(op.conf.c_str(), &cfg) != 0 || cfg.cantidad_nodos == 0)
    {
        fputs("No se pudo cargar sistema.conf o identificar la IP local.\n", stderr);
        return 1;
    }
    if (sistema_init(op.conf.c_str()) != OK)
    {
        fprintf(stderr, "Error iniciando DSM: %d\n", (int)sistema_last_error());
        return 1;
    }
    g_nodo = (int)cfg.nodo_id;
    const uint32_t npag = (uint32_t)(cfg.memoriaNodo / cfg.pageSize);
    const size_t pageSize = cfg.pageSize;
    const uint32_t N = cfg.cantidad_nodos;
    uint32_t miIdx = 0;
    for (uint32_t i = 0; i < N; i++)
        if (cfg.nodos[i].id == cfg.nodo_id)
            miIdx = i;
    const uint32_t id_coord = cfg.nodos[0].id;
    const bool soy_coord = (cfg.nodo_id == id_coord);

    printf("=== app1: Image Filter Engine sobre DSM ===\n");
    printf("Nodo %u (%s) | rol: %s | %u nodo(s) configurados | pagina %zu B | memoria del nodo %s (%u paginas)\n",
           cfg.nodo_id, cfg.ip_local, soy_coord ? "COORDINADOR" : "TRABAJADOR", N, pageSize,
           mib(cfg.memoriaNodo).c_str(), npag);
    printf("Filtro: %s | hilos OpenMP: %d | pasadas: %d | llenar: %.0f%% | escala: x%d\n",
           nombre_filtro(op.filtro), hilos, op.pasadas, op.llenar, op.escala);

    // ---- buzon: N paginas, siempre las primeras (paginas 0..N-1) ----
    vector<void *> buzon(N, NULL);
    for (uint32_t i = 0; i < N; i++)
    {
        buzon[i] = sistema_alloc(pageSize);
        if (!buzon[i])
        {
            fprintf(stderr, "No se pudo reservar el buzon (pagina %u). memoriaNodo es muy chica.\n", i);
            sistema_finalize();
            return 1;
        }
    }
    for (uint32_t i = 0; i < N; i++)
        if (i != miIdx || !soy_coord)
        {
            // El coordinador deja INVALID las paginas ajenas (destino de PAGE_REQUEST).
            // Un trabajador deja INVALID tambien la suya: mientras no termine,
            // nadie puede traerla (el origen rechaza paginas INVALID).
            if (sistema_page_set_state(i, PAGE_INVALID) != OK)
                fprintf(stderr, "Aviso: no se pudo dejar INVALID la pagina %u\n", i);
        }

    if (sistema_red_iniciar() != OK)
    {
        fprintf(stderr, "Error abriendo puertos TCP en %s\n", cfg.ip_local);
        sistema_finalize();
        return 1;
    }
    pthread_t th_cluster, th_monitor;
    pthread_create(&th_cluster, NULL, hilo_cluster, &cfg);
    pthread_create(&th_monitor, NULL, hilo_monitor, NULL);

    // ---- esperar a que se unan todos los nodos ----
    if (N > 1)
    {
        printf("Esperando a los demas nodos (max %d s)...\n", op.esperar);
        double t0 = ahora();
        while (g_continuar && nodos_conectados(cfg) < (int)N && ahora() - t0 < op.esperar)
            dormir_ms(200);
        printf("Nodos conectados: %d/%u%s\n", nodos_conectados(cfg), N,
               nodos_conectados(cfg) < (int)N ? "  (se continua con los disponibles)" : "");
    }

    reporte_memoria("estado inicial (solo buzon reservado)", cfg, npag);

    // ================= FASE 1: CARGA EN DSM =================
    g_fase = FASE_CARGA;
    vector<Fuente> fuentes = cargar_fuentes(dir_in, op.escala);
    printf("\n[CARGA] %zu imagen(es) de origen en %s:\n", fuentes.size(), dir_in.c_str());
    for (auto &f : fuentes)
        printf("        %-30s %dx%d (%s por copia, x2 con resultado)\n", f.nombre.c_str(), f.w, f.h,
               mib((size_t)f.w * f.h * CH).c_str());

    auto redondear = [&](size_t b) { return (b + pageSize - 1) / pageSize * pageSize; };
    const uint64_t presupuesto = (uint64_t)(cfg.memoriaNodo * (op.llenar / 100.0));
    uint64_t usado = (uint64_t)N * pageSize;
    long max_local = 0; // 0 = sin tope
    if (op.imagenes > 0)
        max_local = op.imagenes > (long)miIdx ? (op.imagenes - miIdx + N - 1) / N : 0;

    vector<ImagenDSM> imgs;
    const char *motivo = "completo";
    double t_carga0 = ahora();
    for (long c = 0; g_continuar; c++)
    {
        if (max_local && c >= max_local)
            break;
        uint64_t g = (uint64_t)c * N + miIdx; // reparto round-robin entre nodos
        const Fuente &f = fuentes[g % fuentes.size()];
        size_t bytes = (size_t)f.w * f.h * CH;
        uint64_t costo = 2 * redondear(bytes);
        if (usado + costo > presupuesto)
        {
            motivo = "presupuesto de memoria alcanzado (--llenar)";
            break;
        }
        ImagenDSM im;
        im.w = f.w;
        im.h = f.h;
        im.bytes = bytes;
        im.id_global = g;
        im.in = (unsigned char *)sistema_alloc(bytes);
        im.out = im.in ? (unsigned char *)sistema_alloc(bytes) : NULL;
        if (!im.in || !im.out)
        {
            if (im.in)
                sistema_free(im.in);
            motivo = "DSM sin espacio contiguo";
            break;
        }
        copiar_a_dsm(im.in, f, g / fuentes.size());
        usado += costo;
        imgs.push_back(im);
        g_imagenes = imgs.size();
    }
    double t_carga = ahora() - t_carga0;
    if (imgs.empty())
    {
        fprintf(stderr,
                "\nNo cabe ninguna imagen en la memoria DSM de este nodo.\n"
                "Suba memoriaNodo en sistema.conf (ej. memoriaNodo=268435456 para 256 MiB) o use --escala 1.\n");
        g_continuar = 0;
    }
    uint64_t pixeles_lote = 0, bytes_lote = 0;
    for (auto &im : imgs)
    {
        pixeles_lote += (uint64_t)im.w * im.h;
        bytes_lote += im.bytes * 2;
    }
    printf("\n[CARGA] %zu imagenes en DSM (%s entrada+resultado) en %.2f s -> %s\n", imgs.size(),
           mib(bytes_lote).c_str(), t_carga, motivo);
    reporte_memoria("despues de cargar las imagenes", cfg, npag);

    // ================= FASE 2: PROCESO =================
    double t_proceso = 0, speedup = 0;
    uint64_t pix_procesados = 0;
    if (!imgs.empty() && g_continuar)
    {
        g_fase = FASE_PROCESO;
        if (op.serial && hilos > 1)
        {
            // Misma comparacion serial vs paralelo del main original, sobre la primera imagen.
            // Una pasada previa toca las paginas del resultado por primera vez
            // (si no, el tiempo serial incluiria los fallos de pagina del SO).
            omp_set_num_threads(hilos);
            aplicar_filtro(filtros[0], imgs[0].in, imgs[0].out, imgs[0].w, imgs[0].h);
            omp_set_num_threads(1);
            double a = ahora();
            aplicar_filtro(filtros[0], imgs[0].in, imgs[0].out, imgs[0].w, imgs[0].h);
            double ts = ahora() - a;
            omp_set_num_threads(hilos);
            a = ahora();
            aplicar_filtro(filtros[0], imgs[0].in, imgs[0].out, imgs[0].w, imgs[0].h);
            double tp = ahora() - a;
            speedup = tp > 0 ? ts / tp : 0;
            printf("\n[SPEEDUP] %s en 1 imagen (%dx%d): serial %.4f s | paralelo(%d) %.4f s | %.2fx\n",
                   nombre_filtro(filtros[0]), imgs[0].w, imgs[0].h, ts, hilos, tp, speedup);
        }
        omp_set_num_threads(hilos);
        g_trabajo_total = (uint64_t)imgs.size() * filtros.size() * op.pasadas;
        printf("\n[PROCESO] %zu imagenes x %zu filtro(s) x %d pasada(s)...\n", imgs.size(), filtros.size(),
               op.pasadas);
        double t0 = ahora();
        for (int p = 0; p < op.pasadas && g_continuar; p++)
            for (int f : filtros)
            {
                for (size_t i = 0; i < imgs.size() && g_continuar; i++)
                {
                    aplicar_filtro(f, imgs[i].in, imgs[i].out, imgs[i].w, imgs[i].h);
                    pix_procesados += (uint64_t)imgs[i].w * imgs[i].h;
                    g_trabajo++;
                }
            }
        t_proceso = ahora() - t0;
        printf("[PROCESO] listo en %.2f s | %.1f Mpix/s | %.1f MiB/s leidos+escritos\n", t_proceso,
               t_proceso > 0 ? pix_procesados / 1e6 / t_proceso : 0.0,
               t_proceso > 0 ? (double)pix_procesados * CH * 2 / 1048576.0 / t_proceso : 0.0);
    }

    // checksum de todos los resultados (verifica que los datos en DSM son estables)
    uint64_t checksum = 0;
#pragma omp parallel for reduction(^ : checksum)
    for (long i = 0; i < (long)imgs.size(); i++)
        checksum ^= fnv1a(imgs[i].out, imgs[i].bytes);

    // guardar PNG (como el original: stbi_write_png)
    if (!imgs.empty())
    {
        size_t limite = op.guardarTodas ? imgs.size() : 1;
        for (size_t i = 0; i < limite; i++)
        {
            char ruta[512];
            snprintf(ruta, sizeof(ruta), "%s/app1_nodo%u_%s_img%llu.png", dir_out.c_str(), cfg.nodo_id,
                     op.filtro == 6 ? "todos" : nombre_filtro(op.filtro), (unsigned long long)imgs[i].id_global);
            for (char *p = ruta; *p; p++)
                if (*p == ' ')
                    *p = '_';
            stbi_write_png(ruta, imgs[i].w, imgs[i].h, CH, imgs[i].out, imgs[i].w * CH);
            if (i == 0)
                printf("[SALIDA] %s%s\n", ruta, op.guardarTodas ? " (y las demas)" : "");
        }
    }

    g_fase = FASE_REPORTE;
    reporte_memoria("despues de procesar", cfg, npag);

    // ================= FASE 3: RESULTADOS ENTRE NODOS =================
    // Resumen local.
    Resumen mi;
    memset(&mi, 0, sizeof(mi));
    {
        stats_t s;
        sistema_stats((int)cfg.nodo_id, &s);
        Histograma h = histograma(npag);
        mi.magic = RESUMEN_MAGIC;
        mi.nodo = cfg.nodo_id;
        mi.imagenes = (uint32_t)imgs.size();
        mi.pasadas = (uint32_t)op.pasadas;
        mi.hilos = (uint32_t)hilos;
        mi.filtro = (uint32_t)op.filtro;
        mi.pixeles = pix_procesados;
        mi.dsm_total = s.mem_total;
        mi.dsm_usada = s.mem_usada;
        mi.pag_total = h.total;
        mi.pag_libres = h.libres;
        mi.pag_m = h.modif;
        mi.pag_s = h.shared;
        mi.pag_i = h.inval;
        mi.rss = rss_bytes();
        mi.checksum = checksum;
        mi.faults = s.faults_pagina;
        mi.enviadas = s.paginas_enviadas;
        mi.recibidas = s.paginas_recibidas;
        mi.invalidadas = s.invalidadas;
        mi.bytes_env = s.bytes_enviados;
        mi.bytes_rec = s.bytes_recibidos;
        mi.t_carga = t_carga;
        mi.t_proceso = t_proceso;
        mi.speedup = speedup;
        snprintf(mi.ip, sizeof(mi.ip), "%s", cfg.ip_local);
    }

    if (!soy_coord)
    {
        // TRABAJADOR: libera su pagina de buzon y vuelve a reservar una:
        // al ser la de menor numero libre, cae otra vez en la pagina miIdx y
        // nace MODIFIED. Desde ese momento el coordinador puede traerla.
        g_fase = FASE_PUBLICAR;
        bool publicado = false;
        if (g_continuar && sistema_free(buzon[miIdx]) == OK)
        {
            void *p = sistema_alloc(pageSize);
            page_state_t st;
            if (p && sistema_page_get_state(miIdx, &st) == OK && st == PAGE_MODIFIED)
            {
                memcpy(p, &mi, sizeof(mi));
                buzon[miIdx] = p;
                publicado = true;
                printf("\n[BUZON] Resumen publicado en la pagina %u (estado %s). Esperando al coordinador %u...\n",
                       miIdx, nombre_estado(st), id_coord);
            }
        }
        if (publicado)
        {
            g_fase = FASE_ESPERA;
            // Esperar a que el coordinador haga PAGE_REQUEST (paginas_enviadas >= 1).
            while (g_continuar && nodo_vivo(id_coord))
            {
                stats_t s;
                if (sistema_stats((int)cfg.nodo_id, &s) == OK && s.paginas_enviadas >= 1)
                    break;
                dormir_ms(200);
            }
            page_state_t st;
            if (sistema_page_get_state(miIdx, &st) == OK)
                printf("[MSI] Tras ser leida por el coordinador, la pagina %u paso a %s.\n", miIdx, nombre_estado(st));
            // Ciclo MSI completo: invalidar la copia remota y recuperar escritura.
            status_t r = sistema_red_invalidar_copia(id_coord, miIdx);
            printf("[MSI] INVALIDATE enviado al coordinador: %s\n", r == OK ? "copia invalidada" : "fallo");
            if (r == OK && sistema_page_set_state(miIdx, PAGE_MODIFIED) == OK &&
                sistema_page_get_state(miIdx, &st) == OK)
                printf("[MSI] Sin copias remotas, la pagina %u vuelve a %s (escritura exclusiva).\n", miIdx,
                       nombre_estado(st));
        }
        else if (g_continuar)
            printf("[BUZON] No se pudo publicar el resumen.\n");
        reporte_memoria("final (trabajador)", cfg, npag);
    }
    else
    {
        // COORDINADOR: su propia pagina es el primer resumen; trae las demas.
        g_fase = FASE_ESPERA;
        vector<Resumen> todos(N);
        vector<int> ok(N, 0);
        memcpy(buzon[miIdx], &mi, sizeof(mi));
        todos[miIdx] = mi;
        ok[miIdx] = 1;
        double ultimo_aviso = 0;
        for (uint32_t i = 0; i < N && g_continuar; i++)
        {
            if (i == miIdx)
                continue;
            long intentos = 0;
            while (g_continuar)
            {
                if (!nodo_vivo(cfg.nodos[i].id) && intentos > 0)
                {
                    printf("[BUZON] Nodo %u desconectado; se omite su resultado.\n", cfg.nodos[i].id);
                    break;
                }
                status_t r = sistema_red_pedir_pagina(cfg.nodos[i].id, i);
                intentos++;
                if (r == OK)
                {
                    page_state_t st = PAGE_INVALID;
                    sistema_page_get_state(i, &st);
                    memcpy(&todos[i], buzon[i], sizeof(Resumen));
                    if (todos[i].magic == RESUMEN_MAGIC)
                    {
                        ok[i] = 1;
                        printf("[BUZON] Pagina %u recibida del nodo %u por PAGE_REQUEST (ahora %s aqui).\n", i,
                               cfg.nodos[i].id, nombre_estado(st));
                    }
                    break;
                }
                if (ahora() - ultimo_aviso > 5)
                {
                    printf("[BUZON] Esperando resultados del nodo %u (aun procesando)...\n", cfg.nodos[i].id);
                    ultimo_aviso = ahora();
                }
                dormir_ms(500);
            }
        }

        // ---- reporte global ----
        printf("\n================ REPORTE GLOBAL DEL CLUSTER ================\n");
        printf("%-5s %-15s %8s %16s %9s %9s %9s %9s %8s %8s %s\n", "Nodo", "IP", "Imagenes", "DSM usada/total",
               "Pag M", "Pag S", "Pag I", "RSS", "Carga s", "Proc s", "Checksum");
        uint64_t t_img = 0, t_used = 0, t_tot = 0, t_pix = 0, t_pag = 0, t_env = 0, t_rec = 0, t_inv = 0;
        double max_proc = 0;
        for (uint32_t i = 0; i < N; i++)
        {
            if (!ok[i])
            {
                printf("%-5u %-15s  (sin resultado)\n", cfg.nodos[i].id, cfg.nodos[i].ip);
                continue;
            }
            const Resumen &r = todos[i];
            char uso_s[40];
            snprintf(uso_s, sizeof(uso_s), "%s/%s", mib(r.dsm_usada).c_str(), mib(r.dsm_total).c_str());
            printf("%-5u %-15s %8u %16s %9llu %9llu %9llu %9s %8.2f %8.2f %016llx\n", r.nodo, r.ip, r.imagenes,
                   uso_s, (unsigned long long)r.pag_m, (unsigned long long)r.pag_s, (unsigned long long)r.pag_i,
                   mib(r.rss).c_str(), r.t_carga, r.t_proceso, (unsigned long long)r.checksum);
            t_img += r.imagenes;
            t_used += r.dsm_usada;
            t_tot += r.dsm_total;
            t_pix += r.pixeles;
            t_pag += r.pag_m + r.pag_s + r.pag_i;
            t_env += r.bytes_env;
            t_rec += r.bytes_rec;
            t_inv += r.invalidadas;
            max_proc = max(max_proc, r.t_proceso);
        }
        printf("------------------------------------------------------------\n");
        printf("Memoria DSM del cluster : %s usados de %s (%.1f%%), %llu paginas ocupadas\n", mib(t_used).c_str(),
               mib(t_tot).c_str(), t_tot ? 100.0 * t_used / t_tot : 0.0, (unsigned long long)t_pag);
        printf("Imagenes procesadas     : %llu en total | %.1f Mpix procesados | tiempo del nodo mas lento %.2f s\n",
               (unsigned long long)t_img, t_pix / 1e6, max_proc);
        printf("Rendimiento agregado    : %.1f Mpix/s\n", max_proc > 0 ? t_pix / 1e6 / max_proc : 0.0);
        printf("Trafico DSM (al publicar): enviado %s, recibido %s, invalidaciones %llu\n", mib(t_env).c_str(),
               mib(t_rec).c_str(), (unsigned long long)t_inv);
        printf("============================================================\n");

        // Estado MSI del buzon en el coordinador.
        printf("\n[MSI] Buzon en el coordinador:");
        for (uint32_t i = 0; i < N; i++)
        {
            page_state_t st;
            if (sistema_page_get_state(i, &st) == OK)
                printf("  pagina %u=%s", i, nombre_estado(st));
        }
        printf("\n");
        dormir_ms(800); // dejar que los trabajadores envien su INVALIDATE
        reporte_memoria("final (coordinador)", cfg, npag);
        printf("Tabla de paginas completa: use sistema_print_page_table() o el cliente 'tabla' del nodo.\n");
    }

    // ---- limpieza ----
    g_fase = FASE_INICIO;
    g_monitor_activo = false;
    g_cluster_activo = false;
    pthread_join(th_monitor, NULL);
    pthread_join(th_cluster, NULL);
    for (auto &im : imgs)
    {
        sistema_free(im.in);
        sistema_free(im.out);
    }
    printf("\nLiberadas %zu imagenes de la DSM. Deteniendo nodo...\n", imgs.size());
    if (sistema_finalize() != OK)
    {
        fputs("Error finalizando nodo.\n", stderr);
        return 1;
    }
    return 0;
}