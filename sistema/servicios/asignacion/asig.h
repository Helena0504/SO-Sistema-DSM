#ifndef DSM_ASIG_H
#define DSM_ASIG_H

#include "../../global/global.h"

typedef enum {
    ASIG_OK = 0,
    ASIG_ERROR_PARAMETROS = -1,
    ASIG_ERROR_INICIALIZADO = -2,
    ASIG_ERROR_MEMORIA = -3,
    ASIG_ERROR_SIN_ESPACIO = -4,
    ASIG_ERROR_NO_ENCONTRADO = -5,
    ASIG_ERROR_SISTEMA = -6
} asig_resultado_t;

/* Inicializar estado a cero antes de la primera llamada. */
asig_resultado_t asig_tabla_inicializar(dsm_estado_t *estado,
                                       uint32_t nodo_id,
                                       uint32_t tamano_pagina,
                                       uint64_t memoria_total);

/* Reserva paginas locales consecutivas para un proceso con mmap.
   Devuelve un puntero utilizable; por ahora no hay acceso remoto. */
asig_resultado_t asig_memoria_reservar(dsm_estado_t *estado,
                                      uint64_t proceso_id,
                                      size_t bytes,
                                      void **direccion);

/* Libera una region previamente creada por asig_memoria_reservar. */
asig_resultado_t asig_memoria_liberar(dsm_estado_t *estado,
                                     void *direccion);

/* Libera todas las regiones activas y las dos tablas de asignacion locales;
   no modifica tabla_nodos. */
void asig_tabla_liberar(dsm_estado_t *estado);

#endif
