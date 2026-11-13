#define _POSIX_C_SOURCE 200809L

#include "nodos.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Mutex local al proceso: el intercambio entre maquinas vendra en red.c. */
static pthread_mutex_t mutex_tabla_nodos = PTHREAD_MUTEX_INITIALIZER;

static int nodo_valido(const dsm_nodo_t *nodo)
{
    return nodo != NULL &&
           nodo->nodo_id < DSM_MAX_NODOS &&
           nodo->ip[0] != '\0' &&
           memchr(nodo->ip, '\0', sizeof(nodo->ip)) != NULL &&
           nodo->puerto_pages != 0 &&
           nodo->puerto_nodos != 0 &&
           nodo->puerto_heartbeat != 0 &&
           nodo->memoria_total > 0 &&
           nodo->memoria_libre <= nodo->memoria_total &&
           (nodo->estado == DSM_NODO_CONECTADO ||
            nodo->estado == DSM_NODO_DESCONECTADO);
}

static uint32_t buscar_indice(const dsm_estado_t *estado, uint32_t nodo_id)
{
    for (uint32_t i = 0; i < estado->nodos_conocidos; ++i) {
        if (estado->tabla_nodos[i].nodo_id == nodo_id) {
            return i;
        }
    }
    return DSM_MAX_NODOS;
}

nodos_resultado_t nodos_tabla_inicializar(dsm_estado_t *estado)
{
    if (estado == NULL) {
        return NODOS_ERROR_PARAMETROS;
    }

    pthread_mutex_lock(&mutex_tabla_nodos);
    if (estado->tabla_nodos != NULL || estado->nodos_conocidos != 0) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_YA_INICIALIZADO;
    }

    dsm_nodo_t *tabla = calloc(DSM_MAX_NODOS, sizeof(*tabla));
    if (tabla == NULL) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_MEMORIA;
    }

    estado->tabla_nodos = tabla;
    estado->nodos_conocidos = 0;
    pthread_mutex_unlock(&mutex_tabla_nodos);
    return NODOS_OK;
}

nodos_resultado_t nodos_registrar(dsm_estado_t *estado,
                                 const dsm_nodo_t *nodo)
{
    if (estado == NULL || !nodo_valido(nodo)) {
        return NODOS_ERROR_PARAMETROS;
    }

    pthread_mutex_lock(&mutex_tabla_nodos);
    if (estado->tabla_nodos == NULL) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_SIN_INICIALIZAR;
    }
    if (buscar_indice(estado, nodo->nodo_id) != DSM_MAX_NODOS) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_DUPLICADO;
    }
    if (estado->nodos_conocidos == DSM_MAX_NODOS) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_CAPACIDAD;
    }

    /* Un nodo recien registrado empieza con su hora local de ingreso. */
    uint64_t ahora_ms;
    if (nodos_tiempo_ms(&ahora_ms) != NODOS_OK) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_RELOJ;
    }
    estado->tabla_nodos[estado->nodos_conocidos] = *nodo;
    estado->tabla_nodos[estado->nodos_conocidos].ultimo_heartbeat_ms = ahora_ms;
    ++estado->nodos_conocidos;
    pthread_mutex_unlock(&mutex_tabla_nodos);
    return NODOS_OK;
}

nodos_resultado_t nodos_obtener(const dsm_estado_t *estado,
                               uint32_t nodo_id,
                               dsm_nodo_t *salida)
{
    if (estado == NULL || salida == NULL || nodo_id >= DSM_MAX_NODOS) {
        return NODOS_ERROR_PARAMETROS;
    }

    pthread_mutex_lock(&mutex_tabla_nodos);
    if (estado->tabla_nodos == NULL) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_SIN_INICIALIZAR;
    }
    uint32_t indice = buscar_indice(estado, nodo_id);
    if (indice == DSM_MAX_NODOS) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_NO_ENCONTRADO;
    }

    *salida = estado->tabla_nodos[indice];
    pthread_mutex_unlock(&mutex_tabla_nodos);
    return NODOS_OK;
}

nodos_resultado_t nodos_cambiar_estado(dsm_estado_t *estado,
                                      uint32_t nodo_id,
                                      dsm_estado_nodo_t nuevo_estado)
{
    if (estado == NULL || nodo_id >= DSM_MAX_NODOS ||
        (nuevo_estado != DSM_NODO_CONECTADO &&
         nuevo_estado != DSM_NODO_DESCONECTADO)) {
        return NODOS_ERROR_PARAMETROS;
    }

    pthread_mutex_lock(&mutex_tabla_nodos);
    if (estado->tabla_nodos == NULL) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_SIN_INICIALIZAR;
    }
    uint32_t indice = buscar_indice(estado, nodo_id);
    if (indice == DSM_MAX_NODOS) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_NO_ENCONTRADO;
    }

    estado->tabla_nodos[indice].estado = (uint8_t)nuevo_estado;
    pthread_mutex_unlock(&mutex_tabla_nodos);
    return NODOS_OK;
}

nodos_resultado_t nodos_contar_conectados(const dsm_estado_t *estado,
                                         uint32_t *cantidad)
{
    if (estado == NULL || cantidad == NULL) {
        return NODOS_ERROR_PARAMETROS;
    }

    pthread_mutex_lock(&mutex_tabla_nodos);
    if (estado->tabla_nodos == NULL) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_SIN_INICIALIZAR;
    }

    uint32_t conectados = 0;
    for (uint32_t i = 0; i < estado->nodos_conocidos; ++i) {
        if (estado->tabla_nodos[i].estado == DSM_NODO_CONECTADO) {
            ++conectados;
        }
    }
    *cantidad = conectados;
    pthread_mutex_unlock(&mutex_tabla_nodos);
    return NODOS_OK;
}

void nodos_tabla_liberar(dsm_estado_t *estado)
{
    if (estado == NULL) {
        return;
    }

    pthread_mutex_lock(&mutex_tabla_nodos);
    free(estado->tabla_nodos);
    estado->tabla_nodos = NULL;
    estado->nodos_conocidos = 0;
    pthread_mutex_unlock(&mutex_tabla_nodos);
}


nodos_resultado_t nodos_tiempo_ms(uint64_t *salida_ms)
{
    if (salida_ms == NULL) {
        return NODOS_ERROR_PARAMETROS;
    }

    struct timespec tiempo;
    if (clock_gettime(CLOCK_MONOTONIC, &tiempo) != 0 || tiempo.tv_sec < 0) {
        return NODOS_ERROR_RELOJ;
    }
    *salida_ms = (uint64_t)tiempo.tv_sec * 1000u +
                 (uint64_t)tiempo.tv_nsec / 1000000u;
    return NODOS_OK;
}

nodos_resultado_t nodos_heartbeat_recibido(dsm_estado_t *estado,
                                           uint32_t nodo_id,
                                           uint64_t ahora_ms)
{
    if (estado == NULL || nodo_id >= DSM_MAX_NODOS) {
        return NODOS_ERROR_PARAMETROS;
    }

    pthread_mutex_lock(&mutex_tabla_nodos);
    if (estado->tabla_nodos == NULL) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_SIN_INICIALIZAR;
    }
    uint32_t indice = buscar_indice(estado, nodo_id);
    if (indice == DSM_MAX_NODOS) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_NO_ENCONTRADO;
    }

    dsm_nodo_t *nodo = &estado->tabla_nodos[indice];
    /* Nunca retroceder el tiempo al recibir mensajes atrasados. */
    if (ahora_ms < nodo->ultimo_heartbeat_ms) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_PARAMETROS;
    }
    nodo->ultimo_heartbeat_ms = ahora_ms;
    /* Si estaba caido, debe resincronizar antes de marcarse disponible. */
    pthread_mutex_unlock(&mutex_tabla_nodos);
    return NODOS_OK;
}

nodos_resultado_t nodos_verificar_inactivos(dsm_estado_t *estado,
                                            uint64_t ahora_ms,
                                            uint64_t muerto_h_ms,
                                            uint32_t *nuevos_inactivos)
{
    if (estado == NULL || nuevos_inactivos == NULL || muerto_h_ms == 0) {
        return NODOS_ERROR_PARAMETROS;
    }

    pthread_mutex_lock(&mutex_tabla_nodos);
    if (estado->tabla_nodos == NULL) {
        pthread_mutex_unlock(&mutex_tabla_nodos);
        return NODOS_ERROR_SIN_INICIALIZAR;
    }

    uint32_t caidos = 0;
    for (uint32_t i = 0; i < estado->nodos_conocidos; ++i) {
        dsm_nodo_t *nodo = &estado->tabla_nodos[i];
        /* La maquina local no depende de recibir su propio heartbeat. */
        if (nodo->nodo_id == estado->nodo_id ||
            nodo->estado != DSM_NODO_CONECTADO) {
            continue;
        }
        if (ahora_ms >= nodo->ultimo_heartbeat_ms &&
            ahora_ms - nodo->ultimo_heartbeat_ms >= muerto_h_ms) {
            nodo->estado = DSM_NODO_DESCONECTADO;
            ++caidos;
        }
    }
    *nuevos_inactivos = caidos;
    pthread_mutex_unlock(&mutex_tabla_nodos);
    return NODOS_OK;
}

int nodos_debe_enviar_heartbeat(uint64_t ultimo_envio_ms,
                               uint64_t ahora_ms,
                               uint64_t pausa_h_ms)
{
    if (pausa_h_ms == 0 || ahora_ms < ultimo_envio_ms) {
        return 0;
    }
    /* Sin envio anterior, el primer heartbeat se puede mandar al iniciar. */
    return ultimo_envio_ms == 0 ||
           ahora_ms - ultimo_envio_ms >= pausa_h_ms;
}
