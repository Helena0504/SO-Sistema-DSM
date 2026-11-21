
#define _GNU_SOURCE
#include "asig.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <limits.h>

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
                                       uint64_t memoria_por_nodo,
                                       uint64_t direccion_base,
                                       uint32_t cantidad_nodos)
{
    if (estado == NULL || nodo_id >= cantidad_nodos ||
        cantidad_nodos == 0 || cantidad_nodos > DSM_MAX_NODOS ||
        tamano_pagina == 0 || memoria_por_nodo == 0 ||
        direccion_base == 0 ||
        memoria_por_nodo % tamano_pagina != 0 ||
        !pagina_sistema_compatible(tamano_pagina)) {
        return ASIG_ERROR_PARAMETROS;
    }

    if (estado->tabla_paginas != NULL ||
        estado->tabla_asignaciones != NULL ||
        estado->espacio_global != NULL) {
        return ASIG_ERROR_INICIALIZADO;
    }

    uint64_t paginas_por_nodo = memoria_por_nodo / tamano_pagina;

    if (paginas_por_nodo > UINT32_MAX ||
        paginas_por_nodo > UINT64_MAX / cantidad_nodos) {
        return ASIG_ERROR_PARAMETROS;
    }

    uint64_t cantidad_paginas = paginas_por_nodo * cantidad_nodos;

    if (cantidad_paginas > UINT32_MAX ||
        cantidad_paginas > SIZE_MAX / sizeof(dsm_pagina_t) ||
        cantidad_paginas > SIZE_MAX / sizeof(dsm_asignacion_t) ||
        memoria_por_nodo > UINT64_MAX / cantidad_nodos) {
        return ASIG_ERROR_PARAMETROS;
    }

    uint64_t memoria_global = memoria_por_nodo * cantidad_nodos;

    if (memoria_global > SIZE_MAX ||
        direccion_base > UINTPTR_MAX - memoria_global) {
        return ASIG_ERROR_PARAMETROS;
    }

    size_t bytes_globales = (size_t)memoria_global;

    dsm_pagina_t *tabla =
        calloc((size_t)cantidad_paginas, sizeof(*tabla));

    if (tabla == NULL) {
        return ASIG_ERROR_MEMORIA;
    }

    dsm_asignacion_t *asignaciones =
        calloc((size_t)cantidad_paginas, sizeof(*asignaciones));

    if (asignaciones == NULL) {
        free(tabla);
        return ASIG_ERROR_MEMORIA;
    }

    void *espacio = mmap(
        (void *)(uintptr_t)direccion_base,
        bytes_globales,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
        -1,
        0
    );

    if (espacio == MAP_FAILED ||
        (uintptr_t)espacio != (uintptr_t)direccion_base) {
        if (espacio != MAP_FAILED) {
            munmap(espacio, bytes_globales);
        }

        free(asignaciones);
        free(tabla);
        return ASIG_ERROR_SISTEMA;
    }

    for (uint32_t i = 0; i < (uint32_t)cantidad_paginas; ++i) {
        uint32_t home = i / (uint32_t)paginas_por_nodo;
        reiniciar_pagina(&tabla[i], i, home);
    }

    estado->nodo_id = nodo_id;
    estado->tamano_pagina = tamano_pagina;
    estado->memoria_total = memoria_por_nodo;
    estado->memoria_libre = memoria_por_nodo;
    estado->memoria_global = memoria_global;
    estado->direccion_base = direccion_base;
    estado->memoria_por_nodo = memoria_por_nodo;
    estado->cantidad_nodos = cantidad_nodos;
    estado->paginas_por_nodo = (uint32_t)paginas_por_nodo;
    estado->numero_paginas = (uint32_t)cantidad_paginas;
    estado->espacio_global = espacio;
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
        estado->tabla_asignaciones == NULL ||
        estado->espacio_global == NULL ||
        estado->tamano_pagina == 0 ||
        estado->paginas_por_nodo == 0) {
        return ASIG_ERROR_PARAMETROS;
    }

    size_t tamano = estado->tamano_pagina;
    size_t cantidad = bytes / tamano + (bytes % tamano != 0);

    if (cantidad > estado->paginas_por_nodo ||
        cantidad > estado->memoria_libre / tamano ||
        cantidad > SIZE_MAX / tamano) {
        return ASIG_ERROR_SIN_ESPACIO;
    }

    uint32_t inicio_nodo =
        estado->nodo_id * estado->paginas_por_nodo;

    uint32_t fin_nodo =
        inicio_nodo + estado->paginas_por_nodo;

    if (fin_nodo > estado->numero_paginas) {
        fin_nodo = estado->numero_paginas;
    }

    uint32_t inicio = 0;
    size_t continuas = 0;

    for (uint32_t i = inicio_nodo; i < fin_nodo; ++i) {
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

    uintptr_t base =
        (uintptr_t)estado->espacio_global +
        (uintptr_t)inicio * tamano;

    void *puntero = (void *)base;

    if (mprotect(puntero, bytes_reservados,
                 PROT_READ | PROT_WRITE) != 0) {
        return ASIG_ERROR_SISTEMA;
    }

    for (size_t i = 0; i < cantidad; ++i) {
        dsm_pagina_t *pagina =
            &estado->tabla_paginas[inicio + (uint32_t)i];

        pagina->ocupada = 1;
        pagina->proceso_id = proceso_id;
        pagina->nodo_exclusivo = estado->nodo_id;
        pagina->nodos_con_copia =
            UINT64_C(1) << estado->nodo_id;
        pagina->estado_msi = DSM_MODIFIED;
    }

    estado->tabla_asignaciones[entrada] = (dsm_asignacion_t){
        .proceso_id = proceso_id,
        .region_id = entrada + 1,
        .pagina_inicio = inicio,
        .cantidad_paginas = (uint32_t)cantidad,
        .nodo_solicitante = estado->nodo_id,
        .base_virtual = base
    };

    estado->memoria_libre -= bytes_reservados;
    *direccion = puntero;

    return ASIG_OK;
}

asig_resultado_t asig_memoria_liberar(dsm_estado_t *estado,
                                     void *direccion)
{
    if (estado == NULL || direccion == NULL ||
        estado->tabla_paginas == NULL ||
        estado->tabla_asignaciones == NULL ||
        estado->espacio_global == NULL) {
        return ASIG_ERROR_PARAMETROS;
    }

    for (uint32_t i = 0; i < estado->numero_paginas; ++i) {
        dsm_asignacion_t *asignacion =
            &estado->tabla_asignaciones[i];

        if (asignacion->cantidad_paginas == 0 ||
            asignacion->base_virtual != (uintptr_t)direccion) {
            continue;
        }

        size_t longitud =
            (size_t)asignacion->cantidad_paginas *
            estado->tamano_pagina;

        if (mprotect(direccion, longitud, PROT_NONE) != 0) {
            return ASIG_ERROR_SISTEMA;
        }

        for (uint32_t j = 0; j < asignacion->cantidad_paginas; ++j) {
            uint32_t pagina_id = asignacion->pagina_inicio + j;
            uint32_t home =
                pagina_id / estado->paginas_por_nodo;

            reiniciar_pagina(
                &estado->tabla_paginas[pagina_id],
                pagina_id,
                home
            );
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

    if (estado->espacio_global != NULL &&
        estado->memoria_global != 0) {
        munmap(estado->espacio_global,
               (size_t)estado->memoria_global);
    }

    free(estado->tabla_asignaciones);
    estado->tabla_asignaciones = NULL;

    free(estado->tabla_paginas);
    estado->tabla_paginas = NULL;

    estado->espacio_global = NULL;
    estado->numero_paginas = 0;
    estado->tamano_pagina = 0;
    estado->memoria_total = 0;
    estado->memoria_libre = 0;
    estado->memoria_global = 0;
    estado->direccion_base = 0;
    estado->memoria_por_nodo = 0;
    estado->cantidad_nodos = 0;
    estado->paginas_por_nodo = 0;
}
