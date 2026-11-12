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
    NODOS_ERROR_CAPACIDAD = -7
} nodos_resultado_t;

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

/* Libera exclusivamente la tabla de nodos; no altera las tablas de memoria. */
void nodos_tabla_liberar(dsm_estado_t *estado);

#endif
