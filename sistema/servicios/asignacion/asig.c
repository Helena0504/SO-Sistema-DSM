#define _DEFAULT_SOURCE
#include "asig.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static void reiniciar_pagina(dsm_pagina_t *pagina,
                            uint32_t pagina_id,
                            uint32_t nodo_id)
{
    *pagina = (dsm_pagina_t){0};
    pagina->pagina_id = pagina_id;
    pagina->nodo_origen = nodo_id;
    pagina->nodo_exclusivo = DSM_NODO_SIN_EXCLUSIVIDAD;
    pagina->estado_msi = DSM_INVALID;
}

static int pagina_sistema_compatible(uint32_t tamano_pagina)
{
    long tamano_sistema = sysconf(_SC_PAGESIZE);
    return tamano_sistema > 0 &&
           tamano_pagina >= (uint32_t)tamano_sistema &&
           tamano_pagina % (uint32_t)tamano_sistema == 0;
}

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
    if (estado->tabla_paginas != NULL ||
        estado->tabla_asignaciones != NULL) {
        return ASIG_ERROR_INICIALIZADO;
    }

    uint64_t cantidad = memoria_total / tamano_pagina;
    if (cantidad > UINT32_MAX ||
        cantidad > SIZE_MAX / sizeof(dsm_pagina_t) ||
        cantidad > SIZE_MAX / sizeof(dsm_asignacion_t)) {
        return ASIG_ERROR_PARAMETROS;
    }

    dsm_pagina_t *tabla = calloc((size_t)cantidad, sizeof(*tabla));
    if (tabla == NULL) {
        return ASIG_ERROR_MEMORIA;
    }
    dsm_asignacion_t *asignaciones =
        calloc((size_t)cantidad, sizeof(*asignaciones));
    if (asignaciones == NULL) {
        free(tabla);
        return ASIG_ERROR_MEMORIA;
    }

    for (uint32_t i = 0; i < (uint32_t)cantidad; ++i) {
        reiniciar_pagina(&tabla[i], i, nodo_id);
    }

    estado->nodo_id = nodo_id;
    estado->tamano_pagina = tamano_pagina;
    estado->memoria_total = memoria_total;
    estado->memoria_libre = memoria_total;
    estado->numero_paginas = (uint32_t)cantidad;
    estado->tabla_paginas = tabla;
    estado->tabla_asignaciones = asignaciones;
    return ASIG_OK;
}

asig_resultado_t asig_memoria_reservar(dsm_estado_t *estado,
                                      uint64_t proceso_id,
                                      size_t bytes,
                                      void **direccion)
{
    if (direccion != NULL) {
        *direccion = NULL;
    }
    if (estado == NULL || direccion == NULL || proceso_id == 0 ||
        bytes == 0 || estado->tabla_paginas == NULL ||
        estado->tabla_asignaciones == NULL || estado->tamano_pagina == 0 ||
        !pagina_sistema_compatible(estado->tamano_pagina)) {
        return ASIG_ERROR_PARAMETROS;
    }

    size_t tamano = estado->tamano_pagina;
    size_t cantidad = bytes / tamano + (bytes % tamano != 0);
    if (cantidad > estado->numero_paginas ||
        cantidad > estado->memoria_libre / tamano ||
        cantidad > SIZE_MAX / tamano) {
        return ASIG_ERROR_SIN_ESPACIO;
    }

    /* Elegir un intervalo consecutivo de paginas libres del nodo. */
    uint32_t inicio = 0;
    size_t continuas = 0;
    for (uint32_t i = 0; i < estado->numero_paginas; ++i) {
        if (estado->tabla_paginas[i].ocupada) {
            continuas = 0;
        } else {
            if (continuas == 0) {
                inicio = i;
            }
            ++continuas;
            if (continuas == cantidad) {
                break;
            }
        }
    }
    if (continuas < cantidad) {
        return ASIG_ERROR_SIN_ESPACIO;
    }

    uint32_t entrada = 0;
    for (; entrada < estado->numero_paginas; ++entrada) {
        if (estado->tabla_asignaciones[entrada].cantidad_paginas == 0) {
            break;
        }
    }
    if (entrada == estado->numero_paginas) {
        return ASIG_ERROR_SIN_ESPACIO;
    }

    size_t bytes_reservados = cantidad * tamano;
    void *base = mmap(NULL, bytes_reservados,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return ASIG_ERROR_SISTEMA;
    }

    /* Las paginas locales se crean con acceso exclusivo y contenido en cero.
       El paginador aplicara mas adelante los permisos de MSI remoto. */
    for (size_t i = 0; i < cantidad; ++i) {
        dsm_pagina_t *pagina = &estado->tabla_paginas[inicio + (uint32_t)i];
        pagina->ocupada = 1;
        pagina->proceso_id = proceso_id;
        pagina->nodo_exclusivo = estado->nodo_id;
        pagina->nodos_con_copia = UINT64_C(1) << estado->nodo_id;
        pagina->estado_msi = DSM_MODIFIED;
    }

    estado->tabla_asignaciones[entrada] = (dsm_asignacion_t){
        .proceso_id = proceso_id,
        .region_id = entrada + 1, /* identificador local provisional */
        .pagina_inicio = inicio,
        .cantidad_paginas = (uint32_t)cantidad,
        .nodo_solicitante = estado->nodo_id,
        .base_virtual = (uintptr_t)base
    };
    estado->memoria_libre -= bytes_reservados;
    *direccion = base;
    return ASIG_OK;
}

asig_resultado_t asig_memoria_liberar(dsm_estado_t *estado,
                                     void *direccion)
{
    if (estado == NULL || direccion == NULL ||
        estado->tabla_paginas == NULL ||
        estado->tabla_asignaciones == NULL) {
        return ASIG_ERROR_PARAMETROS;
    }
    for (uint32_t i = 0; i < estado->numero_paginas; ++i) {
        dsm_asignacion_t *asignacion = &estado->tabla_asignaciones[i];
        if (asignacion->cantidad_paginas == 0 ||
            asignacion->base_virtual != (uintptr_t)direccion) {
            continue;
        }

        size_t longitud = (size_t)asignacion->cantidad_paginas *
                          estado->tamano_pagina;
        if (munmap(direccion, longitud) != 0) {
            return ASIG_ERROR_SISTEMA;
        }

        for (uint32_t j = 0; j < asignacion->cantidad_paginas; ++j) {
            uint32_t pagina_id = asignacion->pagina_inicio + j;
            reiniciar_pagina(&estado->tabla_paginas[pagina_id],
                             pagina_id, estado->nodo_id);
        }
        estado->memoria_libre += longitud;
        *asignacion = (dsm_asignacion_t){0};
        return ASIG_OK;
    }
    return ASIG_ERROR_NO_ENCONTRADO;
}

void asig_tabla_liberar(dsm_estado_t *estado)
{
    if (estado == NULL) {
        return;
    }
    if (estado->tabla_asignaciones != NULL) {
        for (uint32_t i = 0; i < estado->numero_paginas; ++i) {
            dsm_asignacion_t *asignacion = &estado->tabla_asignaciones[i];
            if (asignacion->cantidad_paginas != 0) {
                size_t longitud = (size_t)asignacion->cantidad_paginas *
                                  estado->tamano_pagina;
                munmap((void *)asignacion->base_virtual, longitud);
            }
        }
    }
    free(estado->tabla_asignaciones);
    estado->tabla_asignaciones = NULL;
    free(estado->tabla_paginas);
    estado->tabla_paginas = NULL;
    estado->numero_paginas = 0;
    estado->tamano_pagina = 0;
    estado->memoria_total = 0;
    estado->memoria_libre = 0;
}
