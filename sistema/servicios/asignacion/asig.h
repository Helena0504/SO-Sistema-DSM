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

/* El tamano de la tabla logica puede ser mayor que la memoria local.
   Se amplia durante la inicializacion, antes de activar SIGSEGV y TCP. */
asig_resultado_t asig_tabla_ampliar(dsm_estado_t *estado, uint32_t total);

typedef struct {
    uint32_t inicio;
    uint32_t locales;
    uint32_t remotas;
} asig_plan_mixto_t;

/* Reserva una region virtual continua, con paginas finales respaldadas en otro nodo.
   La memoria remota debe haber sido confirmada por MEM_REQUEST. */
asig_resultado_t asig_plan_mixto(const dsm_estado_t *estado, size_t bytes,
                                asig_plan_mixto_t *plan);
asig_resultado_t asig_memoria_reservar_mixta(dsm_estado_t *estado,
                   uint64_t proceso_id, const asig_plan_mixto_t *plan,
                   uint32_t remoto, void **direccion);

/* Reserva/libera almacenamiento real en el nodo que recibe MEM_REQUEST. */
asig_resultado_t asig_reservar_paginas_remotas(dsm_estado_t *estado,
                   uint64_t proceso_id, uint32_t inicio, uint32_t paginas);
asig_resultado_t asig_liberar_paginas_remotas(dsm_estado_t *estado,
                   uint64_t proceso_id, uint32_t inicio, uint32_t paginas);

/* Libera una region previamente creada por asig_memoria_reservar. */
asig_resultado_t asig_memoria_liberar(dsm_estado_t *estado,
                                     void *direccion);

/* Libera todas las regiones activas y las dos tablas de asignacion locales;
   no modifica tabla_nodos. */
void asig_tabla_liberar(dsm_estado_t *estado);

#endif
