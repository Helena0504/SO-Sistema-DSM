#include "pager.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

/* Cada asignacion tiene su propio mmap; la pagina logica puede estar
   dentro de cualquiera de esas regiones. */
static void *direccion_local(const dsm_estado_t *estado, uint32_t pagina_id)
{
    for (uint32_t i = 0; i < estado->numero_paginas; ++i) {
        const dsm_asignacion_t *asig = &estado->tabla_asignaciones[i];
        if (asig->cantidad_paginas == 0 ||
            pagina_id < asig->pagina_inicio ||
            pagina_id - asig->pagina_inicio >= asig->cantidad_paginas) {
            continue;
        }

        size_t desplazamiento = (size_t)(pagina_id - asig->pagina_inicio) *
                                estado->tamano_pagina;
        return (void *)(asig->base_virtual + desplazamiento);
    }
    return NULL;
}

static int permisos_msi(dsm_msi_t msi)
{
    switch (msi) {
        case DSM_INVALID:  return PROT_NONE;
        case DSM_SHARED:   return PROT_READ;
        case DSM_MODIFIED: return PROT_READ | PROT_WRITE;
        default:           return -1;
    }
}

pager_resultado_t pager_cambiar_estado(dsm_estado_t *estado,
                                      uint32_t pagina_id,
                                      dsm_msi_t nuevo_estado)
{
    if (estado == NULL || estado->tabla_paginas == NULL ||
        estado->tabla_asignaciones == NULL || estado->tamano_pagina == 0 ||
        estado->nodo_id >= DSM_MAX_NODOS ||
        pagina_id >= estado->numero_paginas ||
        permisos_msi(nuevo_estado) == -1) {
        return PAGER_ERROR_PARAMETROS;
    }

    dsm_pagina_t *pagina = &estado->tabla_paginas[pagina_id];
    if (!pagina->ocupada) {
        return PAGER_ERROR_NO_ASIGNADA;
    }

    if (pagina->estado_msi == nuevo_estado) {
        return PAGER_OK;
    }

    /* Una pagina invalida no se puede habilitar sin recuperar datos.
       El fetch remoto se agregara cuando este lista la capa de red. */
    if (pagina->estado_msi == DSM_INVALID) {
        return PAGER_ERROR_SIN_DATOS;
    }

    uint64_t copia_local = UINT64_C(1) << estado->nodo_id;
    if (nuevo_estado == DSM_MODIFIED &&
        (pagina->nodos_con_copia & ~copia_local) != 0) {
        return PAGER_ERROR_COPIAS_REMOTAS;
    }

    void *direccion = direccion_local(estado, pagina_id);
    if (direccion == NULL) {
        return PAGER_ERROR_NO_ASIGNADA;
    }

    /* Se cambia primero la proteccion; si Linux falla, MSI queda igual. */
    if (mprotect(direccion, estado->tamano_pagina,
                 permisos_msi(nuevo_estado)) != 0) {
        return PAGER_ERROR_SISTEMA;
    }

    pagina->estado_msi = (uint8_t)nuevo_estado;
    if (nuevo_estado == DSM_MODIFIED) {
        pagina->nodo_exclusivo = estado->nodo_id;
        pagina->nodos_con_copia = copia_local;
    } else {
        pagina->nodo_exclusivo = DSM_NODO_SIN_EXCLUSIVIDAD;
        if (nuevo_estado == DSM_INVALID) {
            pagina->nodos_con_copia &= ~copia_local;
        } else {
            pagina->nodos_con_copia |= copia_local;
        }
    }
    return PAGER_OK;
}

pager_resultado_t pager_consultar_estado(const dsm_estado_t *estado,
                                        uint32_t pagina_id,
                                        dsm_msi_t *salida)
{
    if (estado == NULL || salida == NULL || estado->tabla_paginas == NULL ||
        pagina_id >= estado->numero_paginas) {
        return PAGER_ERROR_PARAMETROS;
    }
    if (!estado->tabla_paginas[pagina_id].ocupada) {
        return PAGER_ERROR_NO_ASIGNADA;
    }
    *salida = (dsm_msi_t)estado->tabla_paginas[pagina_id].estado_msi;
    return PAGER_OK;
}
