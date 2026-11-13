#define _POSIX_C_SOURCE 200809L
#include "pager.h"

#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
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

/* SIGSEGV se configura una sola vez por proceso. No se deben liberar
   las tablas ni las reservas mientras este manejador siga instalado. */
static dsm_estado_t *estado_fallos = NULL;
static struct sigaction manejador_anterior;

/* Devuelve la pagina que contiene la direccion del fallo, si es nuestra. */
static int pagina_del_fallo(const dsm_estado_t *estado,
                           uintptr_t direccion,
                           uint32_t *pagina_id,
                           void **pagina_base)
{
    for (uint32_t i = 0; i < estado->numero_paginas; ++i) {
        const dsm_asignacion_t *asig = &estado->tabla_asignaciones[i];
        if (asig->cantidad_paginas == 0 ||
            direccion < asig->base_virtual) {
            continue;
        }

        uintptr_t diferencia = direccion - asig->base_virtual;
        uint64_t desplazamiento = diferencia / estado->tamano_pagina;
        if (desplazamiento >= asig->cantidad_paginas) {
            continue;
        }

        *pagina_id = asig->pagina_inicio + (uint32_t)desplazamiento;
        *pagina_base = (void *)(asig->base_virtual +
                        (uintptr_t)desplazamiento * estado->tamano_pagina);
        return 1;
    }
    return 0;
}

static void manejar_fallo_pagina(int senal, siginfo_t *info, void *contexto)
{
    (void)senal;
    (void)contexto;
    dsm_estado_t *estado = estado_fallos;
    uint32_t pagina_id;
    void *base;

    /* Solo recuperamos fallos por permisos de paginas locales compartidas.
       Una pagina INVALID necesita datos que todavia no podemos pedir por red. */
    if (estado != NULL && info != NULL && info->si_code == SEGV_ACCERR &&
        pagina_del_fallo(estado, (uintptr_t)info->si_addr,
                         &pagina_id, &base)) {
        dsm_pagina_t *pagina = &estado->tabla_paginas[pagina_id];
        uint64_t copia_local = UINT64_C(1) << estado->nodo_id;

        if (pagina->ocupada && pagina->estado_msi == DSM_SHARED &&
            (pagina->nodos_con_copia & ~copia_local) == 0) {
            /* En este prototipo Linux se ajusta la proteccion al recibir
               la falta. La sincronizacion con otros hilos queda pendiente. */
            if (mprotect(base, estado->tamano_pagina,
                         PROT_READ | PROT_WRITE) == 0) {
                pagina->estado_msi = DSM_MODIFIED;
                pagina->nodo_exclusivo = estado->nodo_id;
                pagina->nodos_con_copia = copia_local;
                return;
            }
        }
    }

    /* Si no podemos resolverlo, devolvemos SIGSEGV al manejador previo.
       Al repetirse el acceso, Linux entregara la falta como de costumbre. */
    (void)sigaction(SIGSEGV, &manejador_anterior, NULL);
    if (info == NULL || info->si_code <= 0) {
        /* Un SIGSEGV enviado por software no repetira una instruccion. */
        (void)raise(SIGSEGV);
    }
}

pager_resultado_t pager_fallos_instalar(dsm_estado_t *estado)
{
    if (estado == NULL || estado->tabla_paginas == NULL ||
        estado->tabla_asignaciones == NULL || estado->tamano_pagina == 0 ||
        estado->nodo_id >= DSM_MAX_NODOS) {
        return PAGER_ERROR_PARAMETROS;
    }
    if (estado_fallos != NULL) {
        return PAGER_ERROR_YA_INSTALADO;
    }

    struct sigaction accion;
    memset(&accion, 0, sizeof(accion));
    accion.sa_sigaction = manejar_fallo_pagina;
    accion.sa_flags = SA_SIGINFO;
    sigemptyset(&accion.sa_mask);

    estado_fallos = estado;
    if (sigaction(SIGSEGV, &accion, &manejador_anterior) != 0) {
        estado_fallos = NULL;
        return PAGER_ERROR_SISTEMA;
    }
    return PAGER_OK;
}

pager_resultado_t pager_fallos_desinstalar(void)
{
    if (estado_fallos == NULL) {
        return PAGER_ERROR_NO_INSTALADO;
    }
    if (sigaction(SIGSEGV, &manejador_anterior, NULL) != 0) {
        return PAGER_ERROR_SISTEMA;
    }
    estado_fallos = NULL;
    return PAGER_OK;
}
