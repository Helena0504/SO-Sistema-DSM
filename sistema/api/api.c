// ============================================================
// api.c
// Implementa el API que coordina los servicios del sistema DSM.
// Integracion con el modulo de estadisticas.
// ============================================================

#include "api.h"
#include "../global/config.h"
#include "../global/logs/logs.h"

#include "../global/global.h"
#include "../servicios/asignacion/asig.h"
#include "../servicios/nodos/nodos.h"
#include "../servicios/paginador/pager.h"
#include "../servicios/estadisticas/stats.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>



// VARIABLES GLOBALES

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



// ERRORES

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



// LOCKS

static void locks_inicializar(void)
{
    for (int i = 0; i < MAX_LOCKS; i++) {
        pthread_mutex_init(&locks[i], NULL);
    }
}


//CONVERSION DE ERRORES NODOS

static status_t convertir_error_nodos(nodos_resultado_t resultado)
{
    switch (resultado) {

        case NODOS_OK:
            return OK;

        case NODOS_ERROR_PARAMETROS:
            return ERROR_ARG;

        case NODOS_ERROR_MEMORIA:
            return ERROR_MEM;

        case NODOS_ERROR_SIN_INICIALIZAR:
        case NODOS_ERROR_YA_INICIALIZADO:
        case NODOS_ERROR_DUPLICADO:
        case NODOS_ERROR_NO_ENCONTRADO:
        case NODOS_ERROR_CAPACIDAD:
        case NODOS_ERROR_RELOJ:
        default:
            return ERROR_NODE;
    }
}



// CONVERSION DE ERRORES


static status_t convertir_error_asig(asig_resultado_t resultado)
{
    switch (resultado) {

        case ASIG_OK:
            return OK;

        case ASIG_ERROR_PARAMETROS:
            return ERROR_ARG;

        case ASIG_ERROR_INICIALIZADO:
            return ERROR_STATE;

        case ASIG_ERROR_MEMORIA:
        case ASIG_ERROR_SIN_ESPACIO:
        case ASIG_ERROR_SISTEMA:
            return ERROR_MEM;

        case ASIG_ERROR_NO_ENCONTRADO:
            return ERROR_ARG;

        default:
            return ERROR_MEM;
    }
}




// CONVERSION DE ERRORES DE ESTADISTICAS

static status_t convertir_error_stats(stats_resultado_t resultado)
{
    switch (resultado) {

        case STATS_OK:
            return OK;

        case STATS_ERROR_PARAMETROS:
            return ERROR_ARG;

        case STATS_ERROR_SIN_INICIALIZAR:
        case STATS_ERROR_YA_INICIALIZADO:
        case STATS_ERROR_BUFFER:
        case STATS_ERROR_RELOJ:
        default:
            return ERROR_STATS;
    }
}


// CONVERSION DE ERRORES DEL PAGER

static status_t convertir_error_pager(pager_resultado_t resultado)
{
    switch (resultado) {

        case PAGER_OK:
            return OK;

        case PAGER_ERROR_PARAMETROS:
        case PAGER_ERROR_NO_ASIGNADA:
            return ERROR_ARG;

        case PAGER_ERROR_SIN_DATOS:
        case PAGER_ERROR_COPIAS_REMOTAS:
            return ERROR_STATE;

        case PAGER_ERROR_SISTEMA:
        case PAGER_ERROR_YA_INSTALADO:
        case PAGER_ERROR_NO_INSTALADO:
        default:
            return ERROR_PAGER;
    }
}



// INICIALIZACION

status_t sistema_init(const char *config_path)
{
    if (config_path == NULL) {
        FALLO(ERROR_ARG);
    }

    pthread_mutex_lock(&mutex_api);

    if (initialized) {
        pthread_mutex_unlock(&mutex_api);
        set_error(OK);
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

        logs_escribir(LOG_ERROR,
                      "Error inicializando asignacion");

        logs_cerrar();

        pthread_mutex_unlock(&mutex_api);

        FALLO(convertir_error_asig(resultado_asig));
    }

    // 4. Inicializar tabla de nodos
    nodos_resultado_t resultado_nodos =
        nodos_tabla_inicializar(&estado_local);

    if (resultado_nodos != NODOS_OK) {

        logs_escribir(LOG_ERROR,
                      "Error inicializando nodos");

        asig_tabla_liberar(&estado_local);
        logs_cerrar();

        pthread_mutex_unlock(&mutex_api);

        FALLO(convertir_error_nodos(resultado_nodos));;
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

        logs_escribir(LOG_ERROR,
                      "Error registrando nodo local");

        nodos_tabla_liberar(&estado_local);
        asig_tabla_liberar(&estado_local);
        logs_cerrar();

        pthread_mutex_unlock(&mutex_api);

        FALLO(convertir_error_nodos(resultado_nodos));;
    }

    // 6. Instalar manejador de fallos de pagina
    pager_resultado_t resultado_pager =
        pager_fallos_instalar(&estado_local);

    if (resultado_pager != PAGER_OK) {

        logs_escribir(
            LOG_ERROR,
            "Error instalando pager. Codigo: %d",
            resultado_pager
        );

        nodos_tabla_liberar(&estado_local);
        asig_tabla_liberar(&estado_local);
        logs_cerrar();

        pthread_mutex_unlock(&mutex_api);

        FALLO(convertir_error_pager(resultado_pager));
    }

    pager_instalado = 1;

    // 7. Inicializar estadisticas
    stats_resultado_t resultado_stats =
        stats_inicializar();

    if (resultado_stats != STATS_OK) {

        logs_escribir(LOG_ERROR,
                      "Error inicializando estadisticas");

        pager_fallos_desinstalar();
        pager_instalado = 0;

        nodos_tabla_liberar(&estado_local);
        asig_tabla_liberar(&estado_local);
        logs_cerrar();

        pthread_mutex_unlock(&mutex_api);

        FALLO(convertir_error_stats(resultado_stats));
    }

    // 8. Inicializar locks
    pthread_once(&locks_once, locks_inicializar);

    // 9. Marcar sistema como inicializado
    initialized = 1;

    logs_escribir(LOG_INFO, "Nodo %u iniciado correctamente", estado_local.nodo_id);

    pthread_mutex_unlock(&mutex_api);

    set_error(OK);

    return OK;
}



// RESERVAR MEMORIA


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

        logs_escribir(
            LOG_ERROR,
            "Error reservando %zu bytes. Codigo ASIG: %d",
            size,
            resultado
        );

        return NULL;
    }

    /* Registrar la solicitud de memoria */
    stats_resultado_t resultado_stats =
        stats_registrar(STATS_SOLICITUD_MEMORIA);

    if (resultado_stats != STATS_OK) {

        /* Revertir la reserva si falla el registro */
        pthread_mutex_lock(&mutex_api);

        asig_memoria_liberar(&estado_local, puntero);

        pthread_mutex_unlock(&mutex_api);

        set_error(ERROR_STATS);

        return NULL;
    }

    logs_escribir(
        LOG_INFO,
        "Reserva de %zu bytes realizada correctamente",
        size
    );

    set_error(OK);

    return puntero;
}


// ESPERAR A QUE LOS NODOS ESTEN LISTOS

status_t sistema_wait_ready(void)
{
    REQUIERE_INIT();

    dsm_nodo_t nodo_local;

    nodos_resultado_t resultado =
        nodos_obtener(
            &estado_local,
            estado_local.nodo_id,
            &nodo_local
        );

    if (resultado != NODOS_OK) {
        FALLO(convertir_error_nodos(resultado));
    }

    if (nodo_local.estado != DSM_NODO_CONECTADO) {
        FALLO(ERROR_NODE);
    }

    set_error(OK);

    return OK;
}



// SINCRONIZACION GLOBAL

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



// LOCK


status_t sistema_lock(int lock_id)
{
    REQUIERE_INIT();

    if (lock_id < 0 || lock_id >= MAX_LOCKS) {
        FALLO(ERROR_ARG);
    }

    if (pthread_mutex_lock(&locks[lock_id]) != 0) {
        FALLO(ERROR_ARG);
    }

    set_error(OK);

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

    set_error(OK);

    return OK;
}



// LIBERAR MEMORIA



status_t sistema_free(void *puntero)
{
    REQUIERE_INIT();

    if (puntero == NULL) {
        FALLO(ERROR_ARG);
    }

    pthread_mutex_lock(&mutex_api);

    asig_resultado_t resultado =
        asig_memoria_liberar(
            &estado_local,
            puntero
        );

    pthread_mutex_unlock(&mutex_api);

    if (resultado != ASIG_OK) {

        status_t error = convertir_error_asig(resultado);

        set_error(error);

        logs_escribir(
            LOG_ERROR,
            "Error liberando memoria. Codigo ASIG: %d",
            resultado
        );

        return error;
    }

    /* Registrar liberacion */
    stats_resultado_t resultado_stats =
        stats_registrar(STATS_LIBERACION_MEMORIA);

    if (resultado_stats != STATS_OK) {
        FALLO(ERROR_STATS);
    }

    logs_escribir(
        LOG_INFO,
        "Memoria liberada correctamente"
    );

    set_error(OK);

    return OK;
}



// FINALIZAR SISTEMA


status_t sistema_finalize(void)
{
    REQUIERE_INIT();

    pthread_mutex_lock(&mutex_api);

    logs_escribir(LOG_INFO, "Finalizando nodo DSM");

    // 1. Desinstalar manejador de fallos
    if (pager_instalado) {

        pager_resultado_t resultado_pager = pager_fallos_desinstalar();

        if (resultado_pager != PAGER_OK) {

            pthread_mutex_unlock(&mutex_api);

            FALLO(convertir_error_pager(resultado_pager));
        }

        pager_instalado = 0;
    }

    // 2. Liberar modulo de estadisticas
    stats_liberar();

    // 3. Liberar tabla de nodos
    nodos_tabla_liberar(&estado_local);

    // 4. Liberar memoria y asignaciones
    asig_tabla_liberar(&estado_local);

    // 5. Marcar sistema como finalizado
    initialized = 0;

    logs_escribir(LOG_INFO, "Sistema DSM finalizado");

    logs_cerrar();

    pthread_mutex_unlock(&mutex_api);

    set_error(OK);

    return OK;
}



// CONSULTAS de IDENTIFICACION DE NODOS

int sistema_node_id(void)
{
    if (!initialized) {
        set_error(ERROR_NO_INIT);
        return -1;
    }

    set_error(OK);

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

    set_error(OK);

    return (int)cantidad;
}



status_t sistema_node_info(int node_id, node_info_t *out)
{
    REQUIERE_INIT();

    if (out == NULL || node_id < 0 ||
        (uint32_t)node_id >= DSM_MAX_NODOS) {
        FALLO(ERROR_ARG);
    }

    dsm_nodo_t nodo;

    nodos_resultado_t resultado =
        nodos_obtener(
            &estado_local,
            (uint32_t)node_id,
            &nodo
        );

    if (resultado != NODOS_OK) {
        FALLO(convertir_error_nodos(resultado));
    }

    memset(out, 0, sizeof(*out));

    out->nodo_id = nodo.nodo_id;

    memcpy(out->ip, nodo.ip, sizeof(out->ip));
    out->ip[sizeof(out->ip) - 1] = '\0';

    out->puerto_pages = nodo.puerto_pages;
    out->puerto_nodos = nodo.puerto_nodos;
    out->puerto_heartbeat = nodo.puerto_heartbeat;

    out->memoria_total = nodo.memoria_total;
    out->memoria_libre = nodo.memoria_libre;

    out->estado = nodo.estado;

    out->ultimo_heartbeat_ms =
        nodo.ultimo_heartbeat_ms;

    set_error(OK);

    return OK;
}


// ESTADISTICAS


status_t sistema_stats(int node_id, stats_t *out)
{
    REQUIERE_INIT();

    if (out == NULL) {
        FALLO(ERROR_ARG);
    }

    // Por ahora solamente se consultan estadisticas locales
    if (node_id != (int)estado_local.nodo_id) {
        FALLO(ERROR_NODE);
    }

    stats_snapshot_t snapshot;

    // Obtener copia consistente de las estadisticas
    stats_resultado_t resultado = stats_obtener(&snapshot);

    if (resultado != STATS_OK) {
        FALLO(convertir_error_stats(resultado));
    }

    memset(out, 0, sizeof(*out));

    
    // Fallos de pagina
    
    out->faults_pagina =
        snapshot.contadores[STATS_FALLO_LECTURA] +
        snapshot.contadores[STATS_FALLO_ESCRITURA];

    
    // Transferencias de paginas
    
    out->paginas_enviadas =
        snapshot.contadores[STATS_PAGINA_ENVIADA];

    out->paginas_recibidas =
        snapshot.contadores[STATS_PAGINA_RECIBIDA];

    
    // Invalidaciones
    
    out->invalidadas =
        snapshot.contadores[STATS_INVALIDACION_ENVIADA] +
        snapshot.contadores[STATS_INVALIDACION_RECIBIDA];

    
    // Operaciones de memoria

    out->solicitudes_memoria =
        snapshot.contadores[STATS_SOLICITUD_MEMORIA];

    out->liberaciones_memoria =
        snapshot.contadores[STATS_LIBERACION_MEMORIA];

    
    // Estadisticas de red

    out->errores_red =
        snapshot.contadores[STATS_ERROR_RED];

    out->bytes_enviados =
        snapshot.contadores[STATS_BYTES_ENVIADOS];

    out->bytes_recibidos =
        snapshot.contadores[STATS_BYTES_RECIBIDOS];

    
    // Tiempo de actividad
    
    out->uptime_ms = snapshot.uptime_ms;

    
    // Latencia de fallos de pagina
    
    out->latencia_muestras = snapshot.latencia_muestras;

    out->latencia_maxima_us = snapshot.latencia_max_us;

    if (snapshot.latencia_muestras > 0) {

        out->latencia_promedio_us =
            snapshot.latencia_total_us /
            snapshot.latencia_muestras;
    }

    
    // Memoria

    out->mem_total = (size_t)estado_local.memoria_total;

    out->mem_usada = (size_t)(
        estado_local.memoria_total >= estado_local.memoria_libre
        ? estado_local.memoria_total - estado_local.memoria_libre
        : 0
    );

    set_error(OK);

    return OK;
}



// IMPRIMIR NODOS


void sistema_print_nodes(void)
{
    if (!initialized) {
        set_error(ERROR_NO_INIT);
        return;
    }

    printf("\n========== NODOS DSM ==========\n");

    uint32_t encontrados = 0;

    for (uint32_t id = 0; id < DSM_MAX_NODOS; ++id) {

        node_info_t nodo;

        status_t resultado =
            sistema_node_info((int)id, &nodo);

        if (resultado != OK) {
            continue;
        }

        printf("Nodo ID: %u\n", nodo.nodo_id);

        printf("IP: %s\n", nodo.ip);

        printf("Puerto Pages: %u\n", nodo.puerto_pages);

        printf("Puerto Nodos: %u\n", nodo.puerto_nodos);

        printf("Puerto Heartbeat: %u\n",
               nodo.puerto_heartbeat);

        printf("Estado: %s\n",
               nodo.estado == DSM_NODO_CONECTADO
               ? "CONECTADO"
               : "DESCONECTADO");

        printf("Memoria total: %llu\n",
               (unsigned long long)nodo.memoria_total);

        printf("Memoria libre: %llu\n",
               (unsigned long long)nodo.memoria_libre);

        printf("Ultimo heartbeat: %llu ms\n",
               (unsigned long long)nodo.ultimo_heartbeat_ms);

        printf("--------------------------------\n");

        encontrados++;
    }

    printf("Nodos encontrados: %u\n", encontrados);

    printf("===============================\n");

    set_error(OK);
}



// IMPRIMIR TABLA DE PAGINAS

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

    set_error(OK);
}



// CAMBIAR ESTADO MSI DE UNA PAGINA

status_t sistema_page_set_state( uint32_t pagina_id, page_state_t nuevo_estado)
{
    REQUIERE_INIT();

    dsm_msi_t estado_pager;

    switch (nuevo_estado) {

        case PAGE_INVALID:
            estado_pager = DSM_INVALID;
            break;

        case PAGE_SHARED:
            estado_pager = DSM_SHARED;
            break;

        case PAGE_MODIFIED:
            estado_pager = DSM_MODIFIED;
            break;

        default:
            FALLO(ERROR_ARG);
    }

    pthread_mutex_lock(&mutex_api);

    pager_resultado_t resultado =
        pager_cambiar_estado(
            &estado_local,
            pagina_id,
            estado_pager
        );

    pthread_mutex_unlock(&mutex_api);

    if (resultado != PAGER_OK) {

        status_t error = convertir_error_pager(resultado);

        logs_escribir(
            LOG_ERROR,
            "Error cambiando estado MSI de pagina %u. Codigo: %d",
            pagina_id,
            resultado
        );

        set_error(error);

        return error;
    }

    logs_escribir(
        LOG_INFO,
        "Estado MSI de pagina %u actualizado",
        pagina_id
    );

    set_error(OK);

    return OK;
}


// CONSULTAR ESTADO MSI DE UNA PAGINA

status_t sistema_page_get_state( uint32_t pagina_id, page_state_t *estado)
{
    REQUIERE_INIT();

    if (estado == NULL) {
        FALLO(ERROR_ARG);
    }

    dsm_msi_t estado_pager;

    pthread_mutex_lock(&mutex_api);

    pager_resultado_t resultado =
        pager_consultar_estado(
            &estado_local,
            pagina_id,
            &estado_pager
        );

    pthread_mutex_unlock(&mutex_api);

    if (resultado != PAGER_OK) {

        FALLO(convertir_error_pager(resultado));
    }

    switch (estado_pager) {

        case DSM_INVALID:
            *estado = PAGE_INVALID;
            break;

        case DSM_SHARED:
            *estado = PAGE_SHARED;
            break;

        case DSM_MODIFIED:
            *estado = PAGE_MODIFIED;
            break;

        default:
            FALLO(ERROR_PAGER);
    }

    set_error(OK);

    return OK;
}


// ULTIMO ERROR

status_t sistema_last_error(void)
{
    pthread_once(&error_once, error_key_create);

    return (status_t)(intptr_t)pthread_getspecific(error_key);
}
