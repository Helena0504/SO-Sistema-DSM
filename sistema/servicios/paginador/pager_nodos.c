#define _POSIX_C_SOURCE 200809L
#include "pager.h"
#include "../nodos/nodos.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

/* El buffer se prepara fuera de red.c para no mezclar TCP con memoria. */
static void *direccion_para_envio(const dsm_estado_t *estado,
                                 uint32_t pagina_id)
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

static pager_resultado_t comprobar_origen(const dsm_estado_t *estado,
                                         uint32_t pagina_id)
{
    if (estado == NULL || estado->tabla_paginas == NULL ||
        estado->tabla_asignaciones == NULL || estado->tamano_pagina == 0 ||
        estado->nodo_id >= DSM_MAX_NODOS ||
        pagina_id >= estado->numero_paginas) {
        return PAGER_ERROR_PARAMETROS;
    }
    if (!estado->tabla_paginas[pagina_id].ocupada) {
        return PAGER_ERROR_NO_ASIGNADA;
    }
    if (estado->tabla_paginas[pagina_id].estado_msi == DSM_INVALID) {
        return PAGER_ERROR_SIN_DATOS;
    }
    return PAGER_OK;
}

/* Solo se toman como destino nodos remotos disponibles en la tabla. */
static pager_resultado_t comprobar_destino(const dsm_estado_t *estado,
                                          uint32_t nodo_destino)
{
    if (nodo_destino >= DSM_MAX_NODOS || nodo_destino == estado->nodo_id) {
        return PAGER_ERROR_PARAMETROS;
    }
    dsm_nodo_t nodo;
    if (nodos_obtener(estado, nodo_destino, &nodo) != NODOS_OK ||
        nodo.estado != DSM_NODO_CONECTADO) {
        return PAGER_ERROR_NODO_NO_DISPONIBLE;
    }
    return PAGER_OK;
}

pager_resultado_t pager_preparar_envio(const dsm_estado_t *estado,
                                      uint32_t pagina_id,
                                      uint32_t nodo_destino,
                                      void *buffer,
                                      size_t capacidad)
{
    if (buffer == NULL) {
        return PAGER_ERROR_PARAMETROS;
    }
    pager_resultado_t resultado = comprobar_origen(estado, pagina_id);
    if (resultado != PAGER_OK) {
        return resultado;
    }
    resultado = comprobar_destino(estado, nodo_destino);
    if (resultado != PAGER_OK) {
        return resultado;
    }
    if (capacidad < estado->tamano_pagina) {
        return PAGER_ERROR_BUFFER_PEQUENO;
    }

    const void *origen = direccion_para_envio(estado, pagina_id);
    if (origen == NULL) {
        return PAGER_ERROR_NO_ASIGNADA;
    }
    /* El envio y la confirmacion TCP se haran desde el modulo de red. */
    memcpy(buffer, origen, estado->tamano_pagina);
    return PAGER_OK;
}

pager_resultado_t pager_confirmar_copia(dsm_estado_t *estado,
                                       uint32_t pagina_id,
                                       uint32_t nodo_destino)
{
    pager_resultado_t resultado = comprobar_origen(estado, pagina_id);
    if (resultado != PAGER_OK) {
        return resultado;
    }
    resultado = comprobar_destino(estado, nodo_destino);
    if (resultado != PAGER_OK) {
        return resultado;
    }

    /* Tras el acuse de recibo, la copia local queda solo de lectura. */
    resultado = pager_cambiar_estado(estado, pagina_id, DSM_SHARED);
    if (resultado != PAGER_OK) {
        return resultado;
    }
    estado->tabla_paginas[pagina_id].nodos_con_copia |=
        UINT64_C(1) << nodo_destino;
    return PAGER_OK;
}

pager_resultado_t pager_confirmar_invalidacion(dsm_estado_t *estado,
                                              uint32_t pagina_id,
                                              uint32_t nodo_destino)
{
    pager_resultado_t resultado = comprobar_origen(estado, pagina_id);
    if (resultado != PAGER_OK) {
        return resultado;
    }
    resultado = comprobar_destino(estado, nodo_destino);
    if (resultado != PAGER_OK) {
        return resultado;
    }

    uint64_t mascara = UINT64_C(1) << nodo_destino;
    dsm_pagina_t *pagina = &estado->tabla_paginas[pagina_id];
    if ((pagina->nodos_con_copia & mascara) == 0) {
        return PAGER_ERROR_COPIA_AUSENTE;
    }
    /* La escritura exclusiva se solicita luego con pager_cambiar_estado. */
    pagina->nodos_con_copia &= ~mascara;
    return PAGER_OK;
}

/* La memoria ya fue reservada con mmap por asig; no se crea otra region. */
pager_resultado_t pager_instalar_copia(dsm_estado_t *estado,
                                      uint32_t pagina_id,
                                      uint32_t nodo_origen,
                                      const void *contenido,
                                      size_t longitud,
                                      uint32_t version)
{
    if (estado == NULL || contenido == NULL ||
        estado->tabla_paginas == NULL ||
        estado->tabla_asignaciones == NULL ||
        pagina_id >= estado->numero_paginas ||
        longitud != estado->tamano_pagina) {
        return PAGER_ERROR_PARAMETROS;
    }

    dsm_pagina_t *pagina = &estado->tabla_paginas[pagina_id];
    if (!pagina->ocupada || pagina->estado_msi != DSM_INVALID) {
        return PAGER_ERROR_NO_ASIGNADA;
    }
    pager_resultado_t r = comprobar_destino(estado, nodo_origen);
    if (r != PAGER_OK) {
        return r;
    }
    void *destino = direccion_para_envio(estado, pagina_id);
    if (destino == NULL) {
        return PAGER_ERROR_NO_ASIGNADA;
    }

    /* Solo se habilita escritura durante la copia. */
    if (mprotect(destino, longitud, PROT_READ | PROT_WRITE) != 0) {
        return PAGER_ERROR_SISTEMA;
    }
    memcpy(destino, contenido, longitud);
    if (mprotect(destino, longitud, PROT_READ) != 0) {
        mprotect(destino, longitud, PROT_NONE);
        return PAGER_ERROR_SISTEMA;
    }

    pagina->estado_msi = DSM_SHARED;
    /* Si vuelve al nodo de origen desde un escritor exclusivo, conservar
       el identificador original y no confundirlo con el emisor temporal. */
    if (!(pagina->nodo_origen == estado->nodo_id &&
          pagina->nodo_exclusivo == nodo_origen)) {
        pagina->nodo_origen = nodo_origen;
    }
    pagina->nodo_exclusivo = DSM_NODO_SIN_EXCLUSIVIDAD;
    pagina->nodos_con_copia = (UINT64_C(1) << estado->nodo_id) |
                              (UINT64_C(1) << nodo_origen);
    pagina->version = version;
    return PAGER_OK;
}

/* El servidor remoto ya invalidó su copia al conceder la exclusividad. */
pager_resultado_t pager_instalar_exclusiva(dsm_estado_t *estado,
                      uint32_t pagina_id, uint32_t nodo_origen,
                      const void *contenido, size_t longitud, uint32_t version)
{
    if (estado == NULL || contenido == NULL ||
        pagina_id >= estado->numero_paginas ||
        longitud != estado->tamano_pagina) return PAGER_ERROR_PARAMETROS;
    dsm_pagina_t *pagina = &estado->tabla_paginas[pagina_id];
    if (!pagina->ocupada || (pagina->estado_msi != DSM_INVALID &&
                             pagina->estado_msi != DSM_SHARED) ||
        comprobar_destino(estado, nodo_origen) != PAGER_OK) {
        return PAGER_ERROR_SIN_DATOS;
    }
    void *destino = direccion_para_envio(estado, pagina_id);
    if (destino == NULL) return PAGER_ERROR_NO_ASIGNADA;
    if (mprotect(destino, longitud, PROT_READ | PROT_WRITE) != 0)
        return PAGER_ERROR_SISTEMA;
    memcpy(destino, contenido, longitud);
    pagina->estado_msi = DSM_MODIFIED;
    pagina->nodo_origen = nodo_origen;
    pagina->nodo_exclusivo = estado->nodo_id;
    pagina->nodos_con_copia = UINT64_C(1) << estado->nodo_id;
    pagina->version = version;
    return PAGER_OK;
}
