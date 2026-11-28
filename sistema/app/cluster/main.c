#define _POSIX_C_SOURCE 200809L
#include "../../api/api.h"
#include "../../global/config.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "http.h"

static volatile sig_atomic_t continuar = 1;

static void terminar(int senal)
{
    (void)senal;
    continuar = 0;
}

static void ayuda(void)
{
    puts("Comandos: nodos | pagina | leer | tabla | tabla ID | tabla global | stats | stats global");
    puts("          pedir ID | invalidar ID | modificar | escribir TEXTO | ayuda | salir");
    puts("Las paginas remotas se recuperan automaticamente al leerlas.");
    puts("Las escrituras invalidan las copias compartidas cuando corresponde.");
}

static const char *nombre_estado(page_state_t estado)
{
    switch (estado) {
        case PAGE_INVALID: return "INVALID";
        case PAGE_SHARED: return "SHARED";
        case PAGE_MODIFIED: return "MODIFIED";
        default: return "DESCONOCIDO";
    }
}

static int leer_id(const char *texto, uint32_t *salida)
{
    if (texto == NULL || *texto == '\0') return -1;
    errno = 0;
    char *fin;
    unsigned long id = strtoul(texto, &fin, 10);
    if (errno != 0 || *fin != '\0' || id >= CONFIG_MAX_NODOS) return -1;
    *salida = (uint32_t)id;
    return 0;
}

static void mostrar_nodos(const config_t *conf)
{
    printf("Nodos conectados: %d/%u\n", sistema_node_cantidad(),
           conf->cantidad_nodos);
    for (uint32_t i = 0; i < conf->cantidad_nodos; i++) {
        const config_nodo_t *esperado = &conf->nodos[i];
        node_info_t info;
        if (sistema_node_info((int)esperado->id, &info) == OK) {
            printf("  Nodo %u  %-16s  %s\n", esperado->id, esperado->ip,
                   info.estado ? "CONECTADO" : "DESCONECTADO");
        } else {
            printf("  Nodo %u  %-16s  SIN CONECTAR\n",
                   esperado->id, esperado->ip);
        }
    }
}

static void mostrar_pagina(const char *pagina)
{
    page_state_t estado;
    if (sistema_page_get_state(0, &estado) != OK) {
        puts("No se pudo consultar la pagina.");
        return;
    }
    printf("Pagina 0: %s\n", nombre_estado(estado));
    if (estado != PAGE_INVALID) {
        printf("Contenido: %s\n", pagina);
    }
}

/* Cada nodo intenta conocer a todos los que figuran en sistema.conf. */

/* Cada nodo intenta conectarse automaticamente con todos los peers configurados. */
static void mantener_cluster(const config_t *conf)
{
    for (uint32_t i = 0; i < conf->cantidad_nodos; i++) {
        const config_nodo_t *remoto = &conf->nodos[i];
        if (remoto->id == conf->nodo_id) continue;

        node_info_t info;
        int conectado = sistema_node_info((int)remoto->id, &info) == OK &&
                        info.estado;

        if (conectado &&
            sistema_red_heartbeat(remoto->id) == OK) {
            continue;
        }

        if (sistema_red_unirse(remoto->ip, conf->puertoNodos) == OK) {
            printf("[RED] Conexion con nodo %u establecida: %s\n",
                   remoto->id, remoto->ip);
        }
    }

    uint32_t caidos = 0;
    if (sistema_red_verificar_nodos(&caidos) == OK && caidos > 0) {
        printf("[RED] %u nodo(s) sin heartbeat.\n", caidos);
    }
}

/* Mantener el heartbeat fuera del hilo que lee los comandos. */
static void *hilo_cluster(void *datos)
{
    const config_t *conf = datos;
    while (continuar) {
        mantener_cluster(conf);
        struct timespec pausa = {
            .tv_sec = (time_t)(conf->pausaH / 1000u),
            .tv_nsec = (long)(conf->pausaH % 1000u) * 1000000L
        };
        nanosleep(&pausa, NULL);
    }
    return NULL;
}

static void ejecutar(const char *linea, const config_t *conf, char *pagina)
{
    if (strcmp(linea, "ayuda") == 0) {
        ayuda();
    } else if (strcmp(linea, "nodos") == 0) {
        mostrar_nodos(conf);
    } else if (strcmp(linea, "pagina") == 0) {
        mostrar_pagina(pagina);
    } else if (strcmp(linea, "leer") == 0) {
        /* El acceso real produce SIGSEGV si la pagina esta INVALID. */
        printf("Lectura de pagina 0: %s\n", pagina);
        mostrar_pagina(pagina);
    } else if (strcmp(linea, "tabla") == 0) {
        sistema_print_page_table((int)conf->nodo_id);
    } else if (strcmp(linea, "tabla global") == 0) {
        sistema_print_global_page_table();
    } else if (strncmp(linea, "tabla ", 6) == 0) {
        uint32_t id;
        if (leer_id(linea + 6, &id) == 0)
            sistema_print_page_table((int)id);
        else puts("Uso: tabla ID | tabla global");
    } else if (strcmp(linea, "stats global") == 0) {
        stats_t total;
        uint32_t consultados;
        if (sistema_stats_global(&total, &consultados) == OK) {
            printf("Nodos consultados: %u\n", consultados);
            printf("Memoria total: %zu, usada: %zu, libre: %zu bytes\n",
                   total.mem_total, total.mem_usada,
                   total.mem_total - total.mem_usada);
            printf("Fallos: %llu, paginas enviadas: %llu, recibidas: %llu\n",
                   (unsigned long long)total.faults_pagina,
                   (unsigned long long)total.paginas_enviadas,
                   (unsigned long long)total.paginas_recibidas);
        } else puts("No se pudieron consultar las estadisticas globales.");
    } else if (strcmp(linea, "stats") == 0) {
        stats_t s;
        if (sistema_stats((int)conf->nodo_id, &s) == OK) {
            printf("Memoria total: %zu, usada: %zu bytes\n", s.mem_total,
                   s.mem_usada);
            printf("Paginas enviadas: %llu, recibidas: %llu, invalidaciones: %llu\n",
                   (unsigned long long)s.paginas_enviadas,
                   (unsigned long long)s.paginas_recibidas,
                   (unsigned long long)s.invalidadas);
            printf("Bytes enviados: %llu, recibidos: %llu\n",
                   (unsigned long long)s.bytes_enviados,
                   (unsigned long long)s.bytes_recibidos);
        } else {
            puts("No se pudieron consultar las estadisticas.");
        }
    } else if (strncmp(linea, "pedir ", 6) == 0) {
        uint32_t id;
        if (leer_id(linea + 6, &id) != 0 || id == conf->nodo_id) {
            puts("Uso: pedir ID_REMOTO");
        } else {
            status_t r = sistema_red_pedir_pagina(id, 0);
            printf("Solicitud de pagina a nodo %u: %s (codigo %d)\n",
                   id, r == OK ? "correcta" : "fallo", (int)r);
            if (r == OK) mostrar_pagina(pagina);
        }
    } else if (strncmp(linea, "invalidar ", 10) == 0) {
        uint32_t id;
        if (leer_id(linea + 10, &id) != 0 || id == conf->nodo_id) {
            puts("Uso: invalidar ID_REMOTO");
        } else {
            status_t r = sistema_red_invalidar_copia(id, 0);
            printf("Invalidacion al nodo %u: %s (codigo %d)\n", id,
                   r == OK ? "correcta" : "fallo", (int)r);
        }
    } else if (strcmp(linea, "modificar") == 0) {
        status_t r = sistema_page_set_state(0, PAGE_MODIFIED);
        printf("Cambio a MODIFIED: %s (codigo %d)\n",
               r == OK ? "correcto" : "denegado", (int)r);
    } else if (strncmp(linea, "escribir ", 9) == 0) {
        page_state_t e;
        if (conf->nodo_id != conf->nodos[0].id ||
            sistema_page_get_state(0, &e) != OK || e == PAGE_INVALID) {
            puts("Solo el nodo origen puede escribir una pagina valida.");
        } else {
            snprintf(pagina, conf->pageSize, "%s", linea + 9);
            puts("Contenido actualizado.");
        }
    } else if (strcmp(linea, "salir") == 0) {
        continuar = 0;
    } else if (*linea != '\0') {
        puts("Comando desconocido. Escriba ayuda.");
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Uso: %s sistema/sistema.conf\n", argv[0]);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stdin, NULL, _IONBF, 0);
    signal(SIGINT, terminar);
    signal(SIGTERM, terminar);

    config_t config;
    if (config_cargar(argv[1], &config) != 0 || config.cantidad_nodos == 0) {
        fputs("No se pudo cargar nodos= o identificar la IP local.\n", stderr);
        return 1;
    }
    if (sistema_init(argv[1]) != OK) {
        fprintf(stderr, "Error iniciando DSM: %d\n", sistema_last_error());
        return 1;
    }
    char *pagina = sistema_alloc(config.pageSize);
    if (pagina == NULL) {
        fputs("No se pudo reservar la pagina local.\n", stderr);
        sistema_finalize();
        return 1;
    }
    uint32_t nodo_origen = config.nodos[0].id;
    if (config.nodo_id == nodo_origen) {
        snprintf(pagina, config.pageSize, "Pagina inicial del nodo %u (%s)",
                 nodo_origen, config.ip_local);
    } else if (sistema_page_set_state(0, PAGE_INVALID) != OK ||
               sistema_page_registrar_origen(0, nodo_origen) != OK) {
        fputs("No se pudo registrar el origen remoto de la pagina.\n", stderr);
        sistema_finalize();
        return 1;
    }
    if (sistema_red_iniciar() != OK) {
        fprintf(stderr, "Error abriendo puertos TCP en %s\n", config.ip_local);
        sistema_finalize();
        return 1;
    }
    if (http_iniciar(&config) != 0) {
        fputs("No se pudo iniciar el servidor HTTP.\n", stderr);
        sistema_finalize();
        return 1;
    }

    printf("DSM iniciado: nodo %u, IP %s, %u nodos configurados\n",
           config.nodo_id, config.ip_local, config.cantidad_nodos);
    printf("Puerto de nodos: %u; de paginas: %u; monitor HTTP: %u\n",
           config.puertoNodos, config.puertoPages,
           (unsigned)config.puertoBase + 5u);
    ayuda();

    pthread_t mantenimiento;
    if (pthread_create(&mantenimiento, NULL, hilo_cluster, &config) != 0) {
        fputs("No se pudo iniciar el hilo de heartbeat.\n", stderr);
        sistema_finalize();
        return 1;
    }

    int entrada_abierta = 1;
    char linea[4096];
    while (continuar) {
        struct pollfd fd = {.fd = 0, .events = POLLIN};
        int r = poll(entrada_abierta ? &fd : NULL,
                     entrada_abierta ? 1 : 0, 150);
        if (r > 0 && (fd.revents & POLLIN)) {
            if (fgets(linea, sizeof(linea), stdin) == NULL) {
                entrada_abierta = 0;
            } else {
                linea[strcspn(linea, "\r\n")] = '\0';
                ejecutar(linea, &config, pagina);
            }
        } else if (r > 0 && (fd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            entrada_abierta = 0;
        }
    }
    pthread_join(mantenimiento, NULL);
	http_detener();
    puts("Deteniendo nodo...");
    if (sistema_finalize() != OK) {
        fputs("Error finalizando nodo.\n", stderr);
        return 1;
    }
    return 0;
}
