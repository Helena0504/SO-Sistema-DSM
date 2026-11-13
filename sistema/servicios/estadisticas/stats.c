// ============================================================
// stats.c
// Contadores de estadisticas de un nodo del sistema DSM.
// Prototipo 1
// ============================================================

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "stats.h"

#include <endian.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

// Estado local
static pthread_mutex_t mutex_stats = PTHREAD_MUTEX_INITIALIZER;
static int inicializado = 0;
static uint64_t contadores[STATS_EVENTO_CANTIDAD];
static uint64_t latencia_total_us = 0;
static uint64_t latencia_max_us = 0;
static uint64_t latencia_muestras = 0;
static uint64_t inicio_ms = 0;

// Reloj del sistema que controla todo sin depender de la hora de una computadora
static stats_resultado_t tiempo_ms(uint64_t *salida_ms)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0 || t.tv_sec < 0) {
        return STATS_ERROR_RELOJ;
    }
    *salida_ms = (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
    return STATS_OK;
}

// Deja todo en cero
static void limpiar_contadores(void)
{
    memset(contadores, 0, sizeof(contadores));
    latencia_total_us = 0;
    latencia_max_us = 0;
    latencia_muestras = 0;
}



//inicializa
stats_resultado_t stats_inicializar(void)
{
    uint64_t ahora;
    stats_resultado_t r = tiempo_ms(&ahora);
    if (r != STATS_OK) {
        return r;
    }

    //usamos mutex para evitar inconsistencias
    pthread_mutex_lock(&mutex_stats);
    if (inicializado) {
        pthread_mutex_unlock(&mutex_stats);
        return STATS_ERROR_YA_INICIALIZADO;
    }
    limpiar_contadores();
    inicio_ms = ahora;
    inicializado = 1;
    pthread_mutex_unlock(&mutex_stats);
    return STATS_OK;
}


//libera todo
void stats_liberar(void)
{
    //usa mutex para evitar inconsistencias
    pthread_mutex_lock(&mutex_stats);
    limpiar_contadores();
    inicio_ms = 0;
    inicializado = 0;
    pthread_mutex_unlock(&mutex_stats);
}


//reinicia
stats_resultado_t stats_reiniciar(void)
{
    uint64_t ahora;
    stats_resultado_t r = tiempo_ms(&ahora);
    if (r != STATS_OK) {
        return r;
    }

    //usar mutex para evitar inconsistencias
    pthread_mutex_lock(&mutex_stats);
    if (!inicializado) {
        pthread_mutex_unlock(&mutex_stats);
        return STATS_ERROR_SIN_INICIALIZAR;
    }
    limpiar_contadores();
    inicio_ms = ahora;
    pthread_mutex_unlock(&mutex_stats);
    return STATS_OK;
}


//suma + 1 o cantidad determinada
stats_resultado_t stats_sumar(stats_evento_t evento, uint64_t cantidad)
{
    if ((int)evento < 0 || evento >= STATS_EVENTO_CANTIDAD) {
        return STATS_ERROR_PARAMETROS;
    }

    //usa mutex para evitar inconsistencias
    pthread_mutex_lock(&mutex_stats);
    if (!inicializado) {
        pthread_mutex_unlock(&mutex_stats);
        return STATS_ERROR_SIN_INICIALIZAR;
    }
    contadores[evento] += cantidad;
    pthread_mutex_unlock(&mutex_stats);
    return STATS_OK;
}


//suma 1
stats_resultado_t stats_registrar(stats_evento_t evento)
{
    return stats_sumar(evento, 1);
}


//sumar tiempo de fallo
stats_resultado_t stats_registrar_latencia_fallo(uint64_t microsegundos)
{
    //usa mutex para evitar inconsistencias
    pthread_mutex_lock(&mutex_stats);
    if (!inicializado) {
        pthread_mutex_unlock(&mutex_stats);
        return STATS_ERROR_SIN_INICIALIZAR;
    }
    latencia_total_us += microsegundos;
    if (microsegundos > latencia_max_us) {
        latencia_max_us = microsegundos;
    }
    ++latencia_muestras;
    pthread_mutex_unlock(&mutex_stats);
    return STATS_OK;
}


//se obtienen stats en un momento determinado
stats_resultado_t stats_obtener(stats_snapshot_t *salida)
{
    if (salida == NULL) {
        return STATS_ERROR_PARAMETROS;
    }

    uint64_t ahora;
    stats_resultado_t r = tiempo_ms(&ahora);
    if (r != STATS_OK) {
        return r;
    }


    //usa mutex para evitar inconsistencias
    pthread_mutex_lock(&mutex_stats);
    if (!inicializado) {
        pthread_mutex_unlock(&mutex_stats);
        return STATS_ERROR_SIN_INICIALIZAR;
    }
    memcpy(salida->contadores, contadores, sizeof(contadores));
    salida->latencia_total_us = latencia_total_us;
    salida->latencia_max_us = latencia_max_us;
    salida->latencia_muestras = latencia_muestras;
    salida->uptime_ms = ahora >= inicio_ms ? ahora - inicio_ms : 0;
    pthread_mutex_unlock(&mutex_stats);
    return STATS_OK;
}

// Ordena el mensaje
stats_resultado_t stats_serializar(const stats_snapshot_t *snap,
                                   void *buffer, size_t capacidad)
{
    if (snap == NULL || buffer == NULL) {
        return STATS_ERROR_PARAMETROS;
    }
    if (capacidad < STATS_SERIAL_BYTES) {
        return STATS_ERROR_BUFFER;
    }

    uint64_t palabras[STATS_SERIAL_PALABRAS];
    size_t k = 0;
    for (int i = 0; i < STATS_EVENTO_CANTIDAD; ++i) {
        palabras[k++] = snap->contadores[i];
    }
    palabras[k++] = snap->latencia_total_us;
    palabras[k++] = snap->latencia_max_us;
    palabras[k++] = snap->latencia_muestras;
    palabras[k++] = snap->uptime_ms;

    for (size_t i = 0; i < STATS_SERIAL_PALABRAS; ++i) {
        palabras[i] = htobe64(palabras[i]);
    }

    memcpy(buffer, palabras, STATS_SERIAL_BYTES);
    return STATS_OK;
}


//vuelve al orden deseado de stats
stats_resultado_t stats_deserializar(const void *buffer, size_t longitud, stats_snapshot_t *salida)
{
    if (buffer == NULL || salida == NULL) {
        return STATS_ERROR_PARAMETROS;
    }
    // El tamano debe ser exacto: otro tamano es otra version del protocolo
    if (longitud != STATS_SERIAL_BYTES) {
        return STATS_ERROR_BUFFER;
    }

    uint64_t palabras[STATS_SERIAL_PALABRAS];
    memcpy(palabras, buffer, STATS_SERIAL_BYTES);

    for (size_t i = 0; i < STATS_SERIAL_PALABRAS; ++i) {
        palabras[i] = be64toh(palabras[i]);
    }

    size_t k = 0;
    for (int i = 0; i < STATS_EVENTO_CANTIDAD; ++i) {
        salida->contadores[i] = palabras[k++];
    }
    salida->latencia_total_us = palabras[k++];
    salida->latencia_max_us = palabras[k++];
    salida->latencia_muestras = palabras[k++];
    salida->uptime_ms = palabras[k++];
    return STATS_OK;
}


//devuelve nombre de stat
const char *stats_nombre_evento(stats_evento_t evento)
{
    switch (evento) {
    case STATS_FALLO_LECTURA:         return "fallos_lectura";
    case STATS_FALLO_ESCRITURA:       return "fallos_escritura";
    case STATS_PAGINA_ENVIADA:        return "paginas_enviadas";
    case STATS_PAGINA_RECIBIDA:       return "paginas_recibidas";
    case STATS_INVALIDACION_ENVIADA:  return "invalidaciones_enviadas";
    case STATS_INVALIDACION_RECIBIDA: return "invalidaciones_recibidas";
    case STATS_SOLICITUD_MEMORIA:     return "solicitudes_memoria";
    case STATS_LIBERACION_MEMORIA:    return "liberaciones_memoria";
    case STATS_HEARTBEAT_ENVIADO:     return "heartbeats_enviados";
    case STATS_HEARTBEAT_RECIBIDO:    return "heartbeats_recibidos";
    case STATS_NODO_CAIDO:            return "nodos_caidos";
    case STATS_ERROR_RED:             return "errores_red";
    case STATS_BYTES_ENVIADOS:        return "bytes_enviados";
    case STATS_BYTES_RECIBIDOS:       return "bytes_recibidos";
    case STATS_EVENTO_CANTIDAD:       break;
    }
    return "desconocido";
}





//solo imprime todo. Mas adelante podemos implementar html para que se vea mas bonito
void stats_imprimir(FILE *salida, int nodo_id, const stats_snapshot_t *snap,
                    uint64_t memoria_total, uint64_t memoria_libre)
{
    if (salida == NULL || snap == NULL) {
        return;
    }

    uint64_t usada = memoria_total >= memoria_libre ? memoria_total - memoria_libre : 0;
    double porcentaje = memoria_total > 0 ? 100.0 * (double)usada / (double)memoria_total : 0.0;
    uint64_t fallos = snap->contadores[STATS_FALLO_LECTURA] +  snap->contadores[STATS_FALLO_ESCRITURA];
    uint64_t latencia_prom = snap->latencia_muestras > 0 ? snap->latencia_total_us / snap->latencia_muestras : 0;

    fprintf(salida, "========== Estadisticas del nodo %d ==========\n", nodo_id);
    fprintf(salida, "Tiempo activo           : %llu ms\n", (unsigned long long)snap->uptime_ms);

    fprintf(salida, "--- Memoria ---\n");
    fprintf(salida, "Total aportada          : %llu bytes\n", (unsigned long long)memoria_total);
    fprintf(salida, "Usada                   : %llu bytes (%.1f%%)\n",
            (unsigned long long)usada, porcentaje);
    fprintf(salida, "Libre                   : %llu bytes\n", (unsigned long long)memoria_libre);
    fprintf(salida, "Solicitudes de memoria  : %llu\n",
            (unsigned long long)snap->contadores[STATS_SOLICITUD_MEMORIA]);
    fprintf(salida, "Liberaciones            : %llu\n",
            (unsigned long long)snap->contadores[STATS_LIBERACION_MEMORIA]);

    fprintf(salida, "--- Paginacion ---\n");
    fprintf(salida, "Fallos de pagina        : %llu (lectura %llu, escritura %llu)\n",
            (unsigned long long)fallos,
            (unsigned long long)snap->contadores[STATS_FALLO_LECTURA],
            (unsigned long long)snap->contadores[STATS_FALLO_ESCRITURA]);
    fprintf(salida, "Paginas enviadas        : %llu\n",
            (unsigned long long)snap->contadores[STATS_PAGINA_ENVIADA]);
    fprintf(salida, "Paginas recibidas       : %llu\n",
            (unsigned long long)snap->contadores[STATS_PAGINA_RECIBIDA]);
    fprintf(salida, "Invalidaciones enviadas : %llu\n",
            (unsigned long long)snap->contadores[STATS_INVALIDACION_ENVIADA]);
    fprintf(salida, "Invalidaciones recibidas: %llu\n",
            (unsigned long long)snap->contadores[STATS_INVALIDACION_RECIBIDA]);
    fprintf(salida, "Latencia de fallo       : promedio %llu us, maxima %llu us (%llu muestras)\n",
            (unsigned long long)latencia_prom,
            (unsigned long long)snap->latencia_max_us,
            (unsigned long long)snap->latencia_muestras);

    fprintf(salida, "--- Red y nodos ---\n");
    fprintf(salida, "Bytes enviados          : %llu\n",
            (unsigned long long)snap->contadores[STATS_BYTES_ENVIADOS]);
    fprintf(salida, "Bytes recibidos         : %llu\n",
            (unsigned long long)snap->contadores[STATS_BYTES_RECIBIDOS]);
    fprintf(salida, "Heartbeats env/recib    : %llu / %llu\n",
            (unsigned long long)snap->contadores[STATS_HEARTBEAT_ENVIADO],
            (unsigned long long)snap->contadores[STATS_HEARTBEAT_RECIBIDO]);
    fprintf(salida, "Nodos caidos detectados : %llu\n",
            (unsigned long long)snap->contadores[STATS_NODO_CAIDO]);
    fprintf(salida, "Errores de red          : %llu\n",
            (unsigned long long)snap->contadores[STATS_ERROR_RED]);
    fprintf(salida, "==============================================\n");
}