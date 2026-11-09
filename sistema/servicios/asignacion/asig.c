#include "asig.h"

#include <stdint.h>
#include <stdlib.h>

asig_resultado_t asig_tabla_inicializar(dsm_estado_t *estado,
                                       uint32_t nodo_id,
                                       uint32_t tamano_pagina,
                                       uint64_t memoria_total)
{
    if (estado == NULL || nodo_id >= DSM_MAX_NODOS ||
        tamano_pagina == 0 || memoria_total == 0 ||
        memoria_total % tamano_pagina != 0) {
        return ASIG_ERROR_PARAMETROS;
    }

    if (estado->tabla_paginas != NULL) {
        return ASIG_ERROR_INICIALIZADO;
    }

    uint64_t cantidad = memoria_total / tamano_pagina;
    if (cantidad > UINT32_MAX ||
        cantidad > SIZE_MAX / sizeof(dsm_pagina_t)) {
        return ASIG_ERROR_PARAMETROS;
    }

    dsm_pagina_t *tabla = calloc((size_t)cantidad, sizeof(*tabla));
    if (tabla == NULL) {
        return ASIG_ERROR_MEMORIA;
    }

    for (uint32_t i = 0; i < (uint32_t)cantidad; ++i) {
        tabla[i].pagina_id = i;
        tabla[i].nodo_origen = nodo_id;
        tabla[i].nodo_exclusivo = DSM_NODO_SIN_EXCLUSIVIDAD;
        tabla[i].estado_msi = DSM_INVALID;
    }

    estado->nodo_id = nodo_id;
    estado->tamano_pagina = tamano_pagina;
    estado->memoria_total = memoria_total;
    estado->memoria_libre = memoria_total;
    estado->numero_paginas = (uint32_t)cantidad;
    estado->tabla_paginas = tabla;

    return ASIG_OK;
}

void asig_tabla_liberar(dsm_estado_t *estado)
{
    if (estado == NULL) {
        return;
    }

    free(estado->tabla_paginas);
    estado->tabla_paginas = NULL;
    estado->numero_paginas = 0;
    estado->tamano_pagina = 0;
    estado->memoria_total = 0;
    estado->memoria_libre = 0;
}
