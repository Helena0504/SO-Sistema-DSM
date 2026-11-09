#ifndef DSM_ASIG_H
#define DSM_ASIG_H

#include "../../global/global.h"

typedef enum {
    ASIG_OK = 0,
    ASIG_ERROR_PARAMETROS = -1,
    ASIG_ERROR_INICIALIZADO = -2,
    ASIG_ERROR_MEMORIA = -3
} asig_resultado_t;

/* El estado debe inicializarse en cero antes de la primera llamada. */
asig_resultado_t asig_tabla_inicializar(dsm_estado_t *estado,
                                       uint32_t nodo_id,
                                       uint32_t tamano_pagina,
                                       uint64_t memoria_total);

/* Libera solo la tabla de paginas; no modifica las otras tablas del nodo. */
void asig_tabla_liberar(dsm_estado_t *estado);

#endif
