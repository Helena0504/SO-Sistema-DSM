#define _DEFAULT_SOURCE
#include "asig.h"

#include <stdint.h>
#include <string.h>
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

static uint32_t paginas_fisicas(const dsm_estado_t *estado)
{
    return (uint32_t)(estado->memoria_total / estado->tamano_pagina);
}

static uint32_t entrada_disponible(const dsm_estado_t *estado)
{
    for (uint32_t i = 0; i < estado->numero_paginas; i++) {
        if (estado->tabla_asignaciones[i].cantidad_paginas == 0) {
            return i;
        }
    }
    return estado->numero_paginas;
}

asig_resultado_t asig_tabla_ampliar(dsm_estado_t *estado, uint32_t total)
{
    if (estado == NULL || estado->tabla_paginas == NULL ||
        estado->tabla_asignaciones == NULL || total < estado->numero_paginas) {
        return ASIG_ERROR_PARAMETROS;
    }
    if (total == estado->numero_paginas) {
        return ASIG_OK;
    }
    dsm_pagina_t *paginas = calloc(total, sizeof(*paginas));
    dsm_asignacion_t *asignaciones = calloc(total, sizeof(*asignaciones));
    if (paginas == NULL || asignaciones == NULL) {
        free(paginas);
        free(asignaciones);
        return ASIG_ERROR_MEMORIA;
    }
    memcpy(paginas, estado->tabla_paginas,
           estado->numero_paginas * sizeof(*paginas));
    memcpy(asignaciones, estado->tabla_asignaciones,
           estado->numero_paginas * sizeof(*asignaciones));
    for (uint32_t i = estado->numero_paginas; i < total; ++i) {
        reiniciar_pagina(&paginas[i], i, estado->nodo_id);
    }
    free(estado->tabla_paginas);
    free(estado->tabla_asignaciones);
    estado->tabla_paginas = paginas;
    estado->tabla_asignaciones = asignaciones;
    estado->numero_paginas = total;
    return ASIG_OK;
}

asig_resultado_t asig_plan_mixto(const dsm_estado_t *estado, size_t bytes,
                                asig_plan_mixto_t *plan)
{
    if (estado == NULL || plan == NULL || bytes == 0 ||
        estado->tabla_paginas == NULL || estado->tamano_pagina == 0 ||
        !pagina_sistema_compatible(estado->tamano_pagina)) {
        return ASIG_ERROR_PARAMETROS;
    }
    size_t tam = estado->tamano_pagina;
    size_t cantidad = bytes / tam + (bytes % tam != 0);
    uint32_t fisicas = paginas_fisicas(estado);
    if (cantidad > UINT32_MAX || cantidad > SIZE_MAX / tam ||
        cantidad > estado->numero_paginas) {
        return ASIG_ERROR_SIN_ESPACIO;
    }
    uint32_t sufijo = 0;
    for (uint32_t i = fisicas; i > 0; --i) {
        if (estado->tabla_paginas[i - 1].ocupada) break;
        ++sufijo;
    }
    uint32_t locales = cantidad < sufijo ? (uint32_t)cantidad : sufijo;
    uint32_t remotas = (uint32_t)cantidad - locales;
    if (remotas == 0 || remotas > estado->numero_paginas - fisicas ||
        (uint64_t)locales * tam > estado->memoria_libre) {
        return ASIG_ERROR_SIN_ESPACIO;
    }
    uint32_t inicio = fisicas - locales;
    int disponible = 1;
    for (uint32_t i = 0; i < remotas; ++i) {
        if (estado->tabla_paginas[fisicas + i].ocupada) disponible = 0;
    }
    if (!disponible) {
        /* Otra region ya ocupa el inicio remoto: reservar una nueva region
           completa en un intervalo remoto libre. */
        locales = 0;
        remotas = (uint32_t)cantidad;
        uint32_t seguidas = 0;
        inicio = estado->numero_paginas;
        for (uint32_t i = fisicas; i < estado->numero_paginas; i++) {
            if (estado->tabla_paginas[i].ocupada) seguidas = 0;
            else if (++seguidas == remotas) {
                inicio = i + 1 - remotas;
                break;
            }
        }
        if (inicio == estado->numero_paginas) return ASIG_ERROR_SIN_ESPACIO;
    }
    if (entrada_disponible(estado) == estado->numero_paginas) {
        return ASIG_ERROR_SIN_ESPACIO;
    }
    *plan = (asig_plan_mixto_t){
        .inicio = inicio,
        .locales = locales,
        .remotas = remotas
    };
    return ASIG_OK;
}

asig_resultado_t asig_memoria_reservar_mixta(dsm_estado_t *estado,
                   uint64_t proceso_id, const asig_plan_mixto_t *plan,
                   uint32_t remoto, void **direccion)
{
    if (direccion != NULL) *direccion = NULL;
    if (estado == NULL || plan == NULL || direccion == NULL ||
        proceso_id == 0 || remoto >= DSM_MAX_NODOS ||
        remoto == estado->nodo_id || plan->remotas == 0 ||
        plan->inicio + (uint64_t)plan->locales + plan->remotas >
        estado->numero_paginas) {
        return ASIG_ERROR_PARAMETROS;
    }
    uint32_t entrada = entrada_disponible(estado);
    if (entrada == estado->numero_paginas) return ASIG_ERROR_SIN_ESPACIO;
    uint32_t paginas = plan->locales + plan->remotas;
    size_t tam = estado->tamano_pagina;
    void *base = mmap(NULL, (size_t)paginas * tam,
                      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return ASIG_ERROR_SISTEMA;
    if (mprotect((char *)base + (size_t)plan->locales * tam,
                 (size_t)plan->remotas * tam, PROT_NONE) != 0) {
        munmap(base, (size_t)paginas * tam);
        return ASIG_ERROR_SISTEMA;
    }
    for (uint32_t i = 0; i < paginas; ++i) {
        dsm_pagina_t *p = &estado->tabla_paginas[plan->inicio + i];
        p->ocupada = 1;
        p->proceso_id = proceso_id;
        if (i < plan->locales) {
            p->estado_msi = DSM_MODIFIED;
            p->nodo_origen = estado->nodo_id;
            p->nodo_exclusivo = estado->nodo_id;
            p->nodos_con_copia = UINT64_C(1) << estado->nodo_id;
        } else {
            p->estado_msi = DSM_INVALID;
            p->nodo_origen = remoto;
            p->nodo_exclusivo = DSM_NODO_SIN_EXCLUSIVIDAD;
            p->nodos_con_copia = 0;
        }
    }
    estado->tabla_asignaciones[entrada] = (dsm_asignacion_t){
        .proceso_id = proceso_id, .region_id = entrada + 1,
        .pagina_inicio = plan->inicio, .cantidad_paginas = paginas,
        .nodo_solicitante = remoto, .base_virtual = (uintptr_t)base
    };
    estado->memoria_libre -= (uint64_t)plan->locales * tam;
    *direccion = base;
    return ASIG_OK;
}

asig_resultado_t asig_reservar_paginas_remotas(dsm_estado_t *estado,
                   uint64_t proceso_id, uint32_t inicio, uint32_t paginas)
{
    if (estado == NULL || estado->tabla_paginas == NULL ||
        estado->tabla_asignaciones == NULL || proceso_id == 0 || paginas == 0 ||
        inicio >= estado->numero_paginas ||
        paginas > estado->numero_paginas - inicio ||
        (uint64_t)paginas * estado->tamano_pagina > estado->memoria_libre) {
        return ASIG_ERROR_SIN_ESPACIO;
    }
    uint32_t entrada = entrada_disponible(estado);
    if (entrada == estado->numero_paginas) return ASIG_ERROR_SIN_ESPACIO;
    for (uint32_t i = 0; i < paginas; ++i) {
        if (estado->tabla_paginas[inicio + i].ocupada) {
            return ASIG_ERROR_SIN_ESPACIO;
        }
    }
    size_t tam = estado->tamano_pagina;
    void *base = mmap(NULL, (size_t)paginas * tam,
                      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return ASIG_ERROR_SISTEMA;
    for (uint32_t i = 0; i < paginas; ++i) {
        dsm_pagina_t *p = &estado->tabla_paginas[inicio + i];
        p->ocupada = 1;
        p->proceso_id = proceso_id;
        p->nodo_origen = estado->nodo_id;
        p->nodo_exclusivo = estado->nodo_id;
        p->estado_msi = DSM_MODIFIED;
        p->nodos_con_copia = UINT64_C(1) << estado->nodo_id;
    }
    estado->tabla_asignaciones[entrada] = (dsm_asignacion_t){
        .proceso_id = proceso_id, .region_id = entrada + 1,
        .pagina_inicio = inicio, .cantidad_paginas = paginas,
        .nodo_solicitante = estado->nodo_id, .base_virtual = (uintptr_t)base
    };
    estado->memoria_libre -= (uint64_t)paginas * tam;
    return ASIG_OK;
}

asig_resultado_t asig_liberar_paginas_remotas(dsm_estado_t *estado,
                   uint64_t proceso_id, uint32_t inicio, uint32_t paginas)
{
    if (estado == NULL || paginas == 0 || estado->tabla_asignaciones == NULL) {
        return ASIG_ERROR_PARAMETROS;
    }
    for (uint32_t i = 0; i < estado->numero_paginas; ++i) {
        const dsm_asignacion_t *a = &estado->tabla_asignaciones[i];
        if (a->cantidad_paginas == paginas && a->pagina_inicio == inicio &&
            a->proceso_id == proceso_id &&
            a->nodo_solicitante == estado->nodo_id) {
            return asig_memoria_liberar(estado, (void *)a->base_virtual);
        }
    }
    return ASIG_ERROR_NO_ENCONTRADO;
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

    /* La asignacion local no invade los IDs logicos de respaldo remoto. */
    uint32_t limite_local = paginas_fisicas(estado);
    uint32_t inicio = 0;
    size_t continuas = 0;
    for (uint32_t i = 0; i < limite_local; ++i) {
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
        if (asignacion->nodo_solicitante != estado->nodo_id) {
            /* Solo el prefijo fisico consumio capacidad del nodo local. */
            uint32_t fisicas = paginas_fisicas(estado);
            uint32_t fin = asignacion->pagina_inicio + asignacion->cantidad_paginas;
            uint32_t limite = fin < fisicas ? fin : fisicas;
            uint32_t propias = limite > asignacion->pagina_inicio
                            ? limite - asignacion->pagina_inicio : 0;
            estado->memoria_libre += (uint64_t)propias * estado->tamano_pagina;
        } else {
            estado->memoria_libre += longitud;
        }
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
