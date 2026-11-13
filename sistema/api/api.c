// ============================================================
// api.c
// Implementa el api que coordina los servicios
///////ESTO SOLO ES UN BORRADOR ///////
///////FALTA LA IMPLEMENTACION DE LOS SERVICIOS ////////
// ============================================================

// ============================================================
// api.c
// Implementa el API que coordina los servicios
// ============================================================

#include "api.h"
#include "config.h"
#include "logs.h"

#include "../global/global.h"
#include "../servicios/asignacion/asig.h"
#include "../servicios/nodos/nodos.h"
#include "../servicios/paginador/pager.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// ============================================================
// VARIABLES GLOBALES
// ============================================================

static int initialized = 0;

static dsm_estado_t estado_local = {0};
static config_t configuracion;

static pthread_mutex_t mutex_api = PTHREAD_MUTEX_INITIALIZER;

// Errores independientes por hilo
static pthread_key_t error_key;
static pthread_once_t error_once = PTHREAD_ONCE_INIT;

static int pager_instalado = 0;

// Locks locales provisionales
#define MAX_LOCKS 256

static pthread_mutex_t locks[MAX_LOCKS];
static pthread_once_t locks_once = PTHREAD_ONCE_INIT;

// ============================================================
// ERRORES
// ============================================================

static void error_key_create(void)
{
    pthread_key_create(&error_key, NULL);
}

static void set_error(status_t e)
{
    pthread_once(&error_once, error_key_create);
    pthread_setspecific(error_key, (void *)(intptr_t)e);
}

#define FALLO(e) do { \
    set_error(e);     \
    return (e);       \
} while (0)

#define REQUIERE_INIT() do { \
    if (!initialized) {      \
        FALLO(ERROR_NO_INIT);\
    }                        \
} while (0)

// ============================================================
// LOCKS
// ============================================================

static void locks_inicializar(void)
{
    for (int i = 0; i < MAX_LOCKS; i++) {
        pthread_mutex_init(&locks[i], NULL);
    }
}

// ============================================================
// CONVERSION DE ERRORES
// ============================================================

static status_t convertir_error_asig(asig_resultado_t resultado)
{
    switch (resultado) {
        case ASIG_OK:
            return OK;

        case ASIG_ERROR_PARAMETROS:
            return ERROR_ARG;

        case ASIG_ERROR_SIN_ESPACIO:
        case ASIG_ERROR_MEMORIA:
            return ERROR_MEM;

        default:
            return ERROR_MEM;
    }
}

// ============================================================
// INICIALIZACION
// ============================================================

status_t sistema_init(const char *config_path)
{
    if (config_path == NULL) {
        FALLO(ERROR_ARG);
    }

    pthread_mutex_lock(&mutex_api);

    if (initialized) {
        pthread_mutex_unlock(&mutex_api);
        return OK;
    }

    // 1. Leer configuracion
    if (config_cargar(config_path, &configuracion) != 0) {
        pthread_mutex_unlock(&mutex_api);
        FALLO(ERROR_CONFIG);
    }

    // 2. Inicializar logs
    if (logs_iniciar(configuracion.logArchivo,
                     configuracion.logLevel) != 0) {
        pthread_mutex_unlock(&mutex_api);
        FALLO(ERROR_CONFIG);
    }

    logs_escribir(LOG_INFO, "Iniciando sistema DSM");

    memset(&estado_local, 0, sizeof(estado_local));

    // 3. Inicializar memoria local
    asig_resultado_t resultado_asig =
        asig_tabla_inicializar(
            &estado_local,
            configuracion.nodo_id,
            configuracion.pageSize,
            configuracion.memoriaNodo
        );

    if (resultado_asig != ASIG_OK) {
        logs_escribir(LOG_ERROR, "Error inicializando asignacion");

        logs_cerrar();
        pthread_mutex_unlock(&mutex_api);

        FALLO(convertir_error_asig(resultado_asig));
    }

    // 4. Inicializar tabla de nodos
    nodos_resultado_t resultado_nodos =
        nodos_tabla_inicializar(&estado_local);

    if (resultado_nodos != NODOS_OK) {
        logs_escribir(LOG_ERROR, "Error inicializando nodos");

        asig_tabla_liberar(&estado_local);
        logs_cerrar();

        pthread_mutex_unlock(&mutex_api);
        FALLO(ERROR_MEM);
    }

    // 5. Registrar el nodo local
    dsm_nodo_t nodo_local = {0};

    nodo_local.nodo_id = configuracion.nodo_id;

    strncpy(nodo_local.ip,
            configuracion.ip_local,
            sizeof(nodo_local.ip) - 1);

    nodo_local.puerto_pages = configuracion.puertoPages;
    nodo_local.puerto_nodos = configuracion.puertoNodos;
    nodo_local.puerto_heartbeat = configuracion.puertoHeartbeat;

    nodo_local.memoria_total = configuracion.memoriaNodo;
    nodo_local.memoria_libre = configuracion.memoriaNodo;

    nodo_local.estado = DSM_NODO_CONECTADO;

    resultado_nodos =
        nodos_registrar(&estado_local, &nodo_local);

    if (resultado_nodos != NODOS_OK) {
        logs_escribir(LOG_ERROR, "Error registrando nodo local");

        nodos_tabla_liberar(&estado_local);
        asig_tabla_liberar(&estado_local);
        logs_cerrar();

        pthread_mutex_unlock(&mutex_api);
        FALLO(ERROR_NODE);
    }

    // 6. Instalar manejador de fallos de pagina
    pager_resultado_t resultado_pager =
        pager_fallos_instalar(&estado_local);

    if (resultado_pager != PAGER_OK) {
        logs_escribir(LOG_ERROR, "Error instalando pager");

        nodos_tabla_liberar(&estado_local);
        asig_tabla_liberar(&estado_local);
        logs_cerrar();

        pthread_mutex_unlock(&mutex_api);
        FALLO(ERROR_MEM);
    }

    pager_instalado = 1;

    // 7. Inicializar locks
    pthread_once(&locks_once, locks_inicializar);

    initialized = 1;

    logs_escribir(LOG_INFO,
                  "Nodo %u iniciado correctamente",
                  estado_local.nodo_id);

    pthread_mutex_unlock(&mutex_api);

    set_error(OK);

    return OK;
}

// ============================================================
// RESERVAR MEMORIA
// ============================================================

void *sistema_alloc(size_t size)
{
    if (!initialized) {
        set_error(ERROR_NO_INIT);
        return NULL;
    }

    if (size == 0) {
        set_error(ERROR_ARG);
        return NULL;
    }

    pthread_mutex_lock(&mutex_api);

    void *puntero = NULL;

    uint64_t proceso_id = (uint64_t)getpid();

    asig_resultado_t resultado =
        asig_memoria_reservar(
            &estado_local,
            proceso_id,
            size,
            &puntero
        );

    pthread_mutex_unlock(&mutex_api);

    if (resultado != ASIG_OK) {
        status_t error = convertir_error_asig(resultado);

        set_error(error);

        logs_escribir(LOG_ERROR,
                      "Error reservando %zu bytes",
                      size);

        return NULL;
    }

    set_error(OK);

    logs_escribir(LOG_INFO,
                  "Reserva de %zu bytes realizada",
                  size);

    return puntero;
}

// ============================================================
// ESPERAR A QUE LOS NODOS ESTEN LISTOS
// ============================================================

status_t sistema_wait_ready(void)
{
    REQUIERE_INIT();

    /*
     * Version 0.0:
     * Solo se registra y verifica el nodo local.
     *
     * La espera distribuida se implementara cuando
     * red.c gestione JOIN_REQUEST y JOIN_RECIBIDO.
     */

    return OK;
}

// ============================================================
// SINCRONIZACION
// ============================================================

status_t sistema_block_all(void)
{
    REQUIERE_INIT();

    /*
     * Pendiente:
     * BLOQUEO_TODOS y BLOQUEO_SUELTO por red.
     *
     * No se simula un bloqueo distribuido.
     */

    FALLO(ERROR_RED);
}

// ============================================================
// LOCK
// ============================================================

status_t sistema_lock(int lock_id)
{
    REQUIERE_INIT();

    if (lock_id < 0 || lock_id >= MAX_LOCKS) {
        FALLO(ERROR_ARG);
    }

    pthread_mutex_lock(&locks[lock_id]);

    return OK;
}

status_t sistema_unlock(int lock_id)
{
    REQUIERE_INIT();

    if (lock_id < 0 || lock_id >= MAX_LOCKS) {
        FALLO(ERROR_ARG);
    }

    if (pthread_mutex_unlock(&locks[lock_id]) != 0) {
        FALLO(ERROR_ARG);
    }

    return OK;
}

// ============================================================
// LIBERAR MEMORIA
// ============================================================

status_t sistema_free(void *puntero)
{
    REQUIERE_INIT();

    if (puntero == NULL) {
        FALLO(ERROR_ARG);
    }

    pthread_mutex_lock(&mutex_api);

    asig_resultado_t resultado =
        asig_memoria_liberar(&estado_local, puntero);

    pthread_mutex_unlock(&mutex_api);

    if (resultado != ASIG_OK) {
        if (resultado == ASIG_ERROR_NO_ENCONTRADO) {
            FALLO(ERROR_ARG);
        }

        FALLO(ERROR_MEM);
    }

    logs_escribir(LOG_INFO, "Memoria liberada correctamente");

    return OK;
}

// ============================================================
// FINALIZAR SISTEMA
// ============================================================

status_t sistema_finalize(void)
{
    REQUIERE_INIT();

    pthread_mutex_lock(&mutex_api);

    logs_escribir(LOG_INFO, "Finalizando nodo DSM");

    // 1. Desinstalar manejador de fallos
    if (pager_instalado) {
        if (pager_fallos_desinstalar() != PAGER_OK) {
            pthread_mutex_unlock(&mutex_api);
            FALLO(ERROR_INTERNAL);
        }

        pager_instalado = 0;
    }

    // 2. Liberar tabla de nodos
    nodos_tabla_liberar(&estado_local);

    // 3. Liberar memoria y asignaciones
    asig_tabla_liberar(&estado_local);

    // 4. Marcar sistema como finalizado
    initialized = 0;

    logs_escribir(LOG_INFO, "Sistema DSM finalizado");

    logs_cerrar();

    pthread_mutex_unlock(&mutex_api);

    return OK;
}

// ============================================================
// CONSULTAS
// ============================================================

int sistema_node_id(void)
{
    if (!initialized) {
        set_error(ERROR_NO_INIT);
        return -1;
    }

    return (int)estado_local.nodo_id;
}

int sistema_node_cantidad(void)
{
    if (!initialized) {
        set_error(ERROR_NO_INIT);
        return -1;
    }

    uint32_t cantidad = 0;

    if (nodos_contar_conectados(&estado_local, &cantidad) != NODOS_OK) {
        set_error(ERROR_NODE);
        return -1;
    }

    return (int)cantidad;
}

// ============================================================
// ESTADISTICAS
// ============================================================

status_t sistema_stats(int node_id, stats_t *out)
{
    REQUIERE_INIT();

    if (out == NULL) {
        FALLO(ERROR_ARG);
    }

    if (node_id != (int)estado_local.nodo_id) {
        FALLO(ERROR_NODE);
    }

    memset(out, 0, sizeof(*out));

    out->mem_total = (size_t)estado_local.memoria_total;

    out->mem_usada = (size_t)(
        estado_local.memoria_total - estado_local.memoria_libre
    );

    return OK;
}

// ============================================================
// IMPRIMIR NODOS
// ============================================================

void sistema_print_nodes(void)
{
    if (!initialized) {
        set_error(ERROR_NO_INIT);
        return;
    }

    printf("\n========== NODOS DSM ==========\n");

    for (uint32_t i = 0; i < estado_local.nodos_conocidos; i++) {
        dsm_nodo_t *nodo = &estado_local.tabla_nodos[i];

        printf("Nodo ID: %u\n", nodo->nodo_id);
        printf("IP: %s\n", nodo->ip);
        printf("Estado: %s\n",
               nodo->estado == DSM_NODO_CONECTADO
               ? "CONECTADO" : "DESCONECTADO");

        printf("Memoria total: %llu\n",
               (unsigned long long)nodo->memoria_total);

        printf("Memoria libre: %llu\n\n",
               (unsigned long long)nodo->memoria_libre);
    }

    printf("===============================\n");
}

// ============================================================
// IMPRIMIR TABLA DE PAGINAS
// ============================================================

void sistema_print_page_table(int node_id)
{
    if (!initialized) {
        set_error(ERROR_NO_INIT);
        return;
    }

    if (node_id != (int)estado_local.nodo_id) {
        set_error(ERROR_NODE);
        return;
    }

    printf("\n========== TABLA DE PAGINAS ==========\n");

    for (uint32_t i = 0; i < estado_local.numero_paginas; i++) {
        dsm_pagina_t *pagina = &estado_local.tabla_paginas[i];

        if (!pagina->ocupada) {
            continue;
        }

        printf("Pagina: %u\n", pagina->pagina_id);
        printf("Proceso: %llu\n",
               (unsigned long long)pagina->proceso_id);

        printf("Estado MSI: %u\n", pagina->estado_msi);

        printf("Nodo origen: %u\n", pagina->nodo_origen);

        printf("Nodo exclusivo: %u\n", pagina->nodo_exclusivo);

        printf("Version: %u\n", pagina->version);

        printf("--------------------------------------\n");
    }

    printf("======================================\n");
}

// ============================================================
// ULTIMO ERROR
// ============================================================

status_t sistema_last_error(void)
{
    pthread_once(&error_once, error_key_create);

    return (status_t)(intptr_t)pthread_getspecific(error_key);
}