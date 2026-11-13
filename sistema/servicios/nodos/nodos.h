#ifndef DSM_NODOS_H
#define DSM_NODOS_H

#include "../../global/global.h"

typedef enum {
    NODOS_OK = 0,
    NODOS_ERROR_PARAMETROS = -1,
    NODOS_ERROR_SIN_INICIALIZAR = -2,
    NODOS_ERROR_YA_INICIALIZADO = -3,
    NODOS_ERROR_MEMORIA = -4,
    NODOS_ERROR_DUPLICADO = -5,
    NODOS_ERROR_NO_ENCONTRADO = -6,
    NODOS_ERROR_CAPACIDAD = -7,
    NODOS_ERROR_RELOJ = -8
} nodos_resultado_t;

#define NODOS_PAUSA_H_DEFAULT 1000u
#define NODOS_MUERTO_H_DEFAULT 3000u

/* Crea la tabla local donde se registraran hasta DSM_MAX_NODOS nodos. */
nodos_resultado_t nodos_tabla_inicializar(dsm_estado_t *estado);

/* Agrega un nodo con identificador unico; no realiza conexiones de red. */
nodos_resultado_t nodos_registrar(dsm_estado_t *estado,
                                 const dsm_nodo_t *nodo);

/* Copia un registro en salida para no exponer la tabla durante escrituras. */
nodos_resultado_t nodos_obtener(const dsm_estado_t *estado,
                               uint32_t nodo_id,
                               dsm_nodo_t *salida);

/* Cambia la disponibilidad registrada de un nodo ya conocido. */
nodos_resultado_t nodos_cambiar_estado(dsm_estado_t *estado,
                                      uint32_t nodo_id,
                                      dsm_estado_nodo_t nuevo_estado);

/* Cuenta solo los nodos actualmente registrados como conectados. */
nodos_resultado_t nodos_contar_conectados(const dsm_estado_t *estado,
                                         uint32_t *cantidad);

/* Tiempo local en milisegundos usando CLOCK_MONOTONIC. */
nodos_resultado_t nodos_tiempo_ms(uint64_t *salida_ms);

/* Guarda un HEARTBEAT valido; no reconecta nodos caidos sin sincronizar. */
nodos_resultado_t nodos_heartbeat_recibido(dsm_estado_t *estado,
                                           uint32_t nodo_id,
                                           uint64_t ahora_ms);

/* Marca nodos remotos sin heartbeat dentro del plazo configurado. */
nodos_resultado_t nodos_verificar_inactivos(dsm_estado_t *estado,
                                            uint64_t ahora_ms,
                                            uint64_t muerto_h_ms,
                                            uint32_t *nuevos_inactivos);

/* Indica si ya corresponde enviar el siguiente heartbeat. */
int nodos_debe_enviar_heartbeat(uint64_t ultimo_envio_ms,
                               uint64_t ahora_ms,
                               uint64_t pausa_h_ms);

/* Libera exclusivamente la tabla de nodos; no altera las tablas de memoria. */
void nodos_tabla_liberar(dsm_estado_t *estado);

#endif
