#ifndef DSM_PUENTE_H
#define DSM_PUENTE_H

#include "red.h"
#include "../global/config.h"
#include "../global/global.h"
#include "../servicios/estadisticas/stats.h"
#include <semaphore.h>

/* Conecta los mensajes TCP con las tablas locales del DSM. */
typedef enum {
    PUENTE_OK = 0,
    PUENTE_ERROR_PARAMETROS = -1,
    PUENTE_ERROR_RED = -2,
    PUENTE_ERROR_NODO = -3,
    PUENTE_ERROR_PAGINA = -4,
    PUENTE_ERROR_RESPUESTA = -5,
    PUENTE_ERROR_INICIADO = -6,
    PUENTE_ERROR_OCUPADO = -7
} puente_resultado_t;

#define PUENTE_MAX_LOCKS 256

typedef struct {
    dsm_estado_t *estado;
    config_t config;
    red_servidor_t servidor_nodos;
    red_servidor_t servidor_heartbeat;
    red_servidor_t servidor_paginas;
    red_servidor_t servidor_consultas;
    pthread_mutex_t mutex;
    /* El nodo 0 arbitra los locks, usando semaforos POSIX. */
    pthread_mutex_t mutex_sincronizacion;
    sem_t locks_central[PUENTE_MAX_LOCKS];
    uint32_t propietario_lock[PUENTE_MAX_LOCKS];
    uint32_t generacion_barrera;
    uint32_t generacion_local;
    uint64_t llegadas_barrera;
    int iniciado;
} puente_t;

/* Abre los puertos configurados. El estado y las tablas deben existir. */
puente_resultado_t puente_iniciar(puente_t *puente, dsm_estado_t *estado,
                                 const config_t *config);
void puente_detener(puente_t *puente);

/* Registra ambos extremos usando JOIN_REQUEST/JOIN_RECIBIDO. */
puente_resultado_t puente_unirse(puente_t *puente, const char *ip,
                                uint16_t puerto_nodos);

/* Mensajes que se pueden solicitar desde una aplicacion o desde la API. */
puente_resultado_t puente_enviar_heartbeat(puente_t *puente, uint32_t destino);
puente_resultado_t puente_pedir_pagina(puente_t *puente, uint32_t origen,
                                      uint32_t pagina_id);
puente_resultado_t puente_invalidar_copia(puente_t *puente, uint32_t destino,
                                          uint32_t pagina_id);
puente_resultado_t puente_pedir_escritura(puente_t *puente, uint32_t origen,
                                         uint32_t pagina_id);

/* Reserva y libera paginas de respaldo utilizando MEM_REQUEST y MEM_FREE. */
puente_resultado_t puente_reservar_memoria(puente_t *puente, uint32_t nodo,
                       uint64_t proceso_id, uint32_t inicio, uint32_t paginas);
puente_resultado_t puente_liberar_memoria(puente_t *puente, uint32_t nodo,
                       uint64_t proceso_id, uint32_t inicio, uint32_t paginas);

/* Sincronizacion global coordinada por el nodo 0. */
puente_resultado_t puente_lock_global(puente_t *puente, uint32_t lock_id);
puente_resultado_t puente_unlock_global(puente_t *puente, uint32_t lock_id);
puente_resultado_t puente_barrera_global(puente_t *puente);

/* Solicita contadores al otro nodo mediante QUERY_STATS. */
puente_resultado_t puente_consultar_stats(puente_t *puente, uint32_t remoto,
                                         uint16_t puerto_consulta,
                                         stats_snapshot_t *salida);

/* Consulta la tabla de paginas ocupadas y el estado actual de un nodo remoto.
   La persona que llama libera *paginas con free(). */
puente_resultado_t puente_consultar_paginas(puente_t *puente, uint32_t nodo,
                       dsm_pagina_t **paginas, size_t *cantidad);
puente_resultado_t puente_consultar_nodo(puente_t *puente, uint32_t nodo,
                                         dsm_nodo_t *info);

/* Aplica muertoH a los nodos remotos registrados. */
puente_resultado_t puente_verificar_heartbeat(puente_t *puente,
                                               uint32_t *nodos_caidos);

#endif
