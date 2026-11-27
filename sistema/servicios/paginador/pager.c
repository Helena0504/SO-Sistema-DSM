#define _POSIX_C_SOURCE 200809L
#include "pager.h"

#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

pager_resultado_t pager_registrar_origen(dsm_estado_t *estado,
                                        uint32_t pagina_id,
                                        uint32_t nodo_origen)
{
    if (estado == NULL || estado->tabla_paginas == NULL ||
        pagina_id >= estado->numero_paginas ||
        nodo_origen >= DSM_MAX_NODOS || nodo_origen == estado->nodo_id) {
        return PAGER_ERROR_PARAMETROS;
    }
    dsm_pagina_t *pagina = &estado->tabla_paginas[pagina_id];
    if (!pagina->ocupada || pagina->estado_msi != DSM_INVALID) {
        return PAGER_ERROR_SIN_DATOS;
    }
    pagina->nodo_origen = nodo_origen;
    pagina->nodo_exclusivo = DSM_NODO_SIN_EXCLUSIVIDAD;
    pagina->nodos_con_copia &= ~(UINT64_C(1) << estado->nodo_id);
    return PAGER_OK;
}

/* La asociacion se realiza por region, no pagina por pagina. La pagina
   conserva su identificador logico y sera recuperada al primer acceso. */
pager_resultado_t pager_region_remota(dsm_estado_t *estado,
                                     void *base, uint32_t nodo_origen)
{
    if (estado == NULL || base == NULL ||
        estado->tabla_paginas == NULL || estado->tabla_asignaciones == NULL ||
        estado->tamano_pagina == 0 || nodo_origen >= DSM_MAX_NODOS ||
        nodo_origen == estado->nodo_id) {
        return PAGER_ERROR_PARAMETROS;
    }

    const dsm_asignacion_t *region = NULL;
    for (uint32_t i = 0; i < estado->numero_paginas; ++i) {
        const dsm_asignacion_t *actual = &estado->tabla_asignaciones[i];
        if (actual->cantidad_paginas != 0 &&
            actual->base_virtual == (uintptr_t)base) {
            region = actual;
            break;
        }
    }
    if (region == NULL) {
        return PAGER_ERROR_NO_ASIGNADA;
    }

    /* Comprobar toda la reserva antes de cambiar permisos o metadatos. */
    const uint64_t local = UINT64_C(1) << estado->nodo_id;
    for (uint32_t i = 0; i < region->cantidad_paginas; ++i) {
        const dsm_pagina_t *pagina =
            &estado->tabla_paginas[region->pagina_inicio + i];
        if (!pagina->ocupada || pagina->estado_msi != DSM_MODIFIED ||
            pagina->nodo_origen != estado->nodo_id ||
            pagina->nodos_con_copia != local) {
            return PAGER_ERROR_SIN_DATOS;
        }
    }

    /* Todas las paginas del mismo mmap se protegen en una unica operacion. */
    size_t longitud = (size_t)region->cantidad_paginas * estado->tamano_pagina;
    if (mprotect(base, longitud, PROT_NONE) != 0) {
        return PAGER_ERROR_SISTEMA;
    }
    for (uint32_t i = 0; i < region->cantidad_paginas; ++i) {
        dsm_pagina_t *pagina =
            &estado->tabla_paginas[region->pagina_inicio + i];
        pagina->estado_msi = DSM_INVALID;
        pagina->nodo_origen = nodo_origen;
        pagina->nodo_exclusivo = DSM_NODO_SIN_EXCLUSIVIDAD;
        pagina->nodos_con_copia = 0;
    }
    return PAGER_OK;
}

/* El manejador solo comunica el fallo mediante pipes. La red, los mutexes y
   mprotect se ejecutan en un hilo normal. Se atiende un fallo a la vez. */
static dsm_estado_t *estado_fallos = NULL;
static struct sigaction manejador_anterior;
static int canal_pedidos[2] = {-1, -1};
static int canal_respuestas[2] = {-1, -1};
static pthread_t hilo_resolucion;
static volatile sig_atomic_t fallo_pendiente = 0;
static volatile sig_atomic_t pagina_pendiente = 0;
static volatile sig_atomic_t tipo_pendiente = DSM_INVALID;
static pager_resolver_remoto_t resolver_remoto = NULL;
static void *contexto_remoto = NULL;

void pager_fallos_configurar_resolucion(pager_resolver_remoto_t resolver,
                                       void *contexto)
{
    resolver_remoto = resolver;
    contexto_remoto = contexto;
}

static void *hilo_resolver_fallos(void *arg)
{
    dsm_estado_t *estado = arg;
    char mensaje;
    while (read(canal_pedidos[0], &mensaje, 1) == 1) {
        if (mensaje == 'F') {
            uint32_t pagina_id = (uint32_t)pagina_pendiente;
            dsm_msi_t msi = (dsm_msi_t)tipo_pendiente;
            char resultado = 'N';
            uint64_t local = UINT64_C(1) << estado->nodo_id;
            if (pagina_id < estado->numero_paginas) {
                dsm_pagina_t *pagina = &estado->tabla_paginas[pagina_id];
                if (msi == DSM_SHARED && pagina->ocupada &&
                    pagina->estado_msi == DSM_SHARED &&
                    (pagina->nodos_con_copia & ~local) == 0) {
                    if (pager_cambiar_estado(estado, pagina_id, DSM_MODIFIED) == PAGER_OK) {
                        resultado = 'S';
                    }
                } else if (resolver_remoto != NULL &&
                           resolver_remoto(contexto_remoto, pagina_id, msi) == 0) {
                    resultado = 'S';
                }
            }
            (void)write(canal_respuestas[1], &resultado, 1);
        } else if (mensaje == 'Q') {
            break;
        }
    }
    return NULL;
}

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

    if (estado != NULL && info != NULL && info->si_code == SEGV_ACCERR &&
        pagina_del_fallo(estado, (uintptr_t)info->si_addr,
                         &pagina_id, &base)) {
        (void)base;
        dsm_pagina_t *pagina = &estado->tabla_paginas[pagina_id];
        uint64_t copia_local = UINT64_C(1) << estado->nodo_id;
        dsm_msi_t previo = (dsm_msi_t)pagina->estado_msi;
        int local_sin_copias = previo == DSM_SHARED &&
                (pagina->nodos_con_copia & ~copia_local) == 0;
        int remoto = resolver_remoto != NULL &&
                (previo == DSM_INVALID || previo == DSM_SHARED);

        if (pagina->ocupada && (local_sin_copias || remoto)) {
            /* Una sola solicitud activa evita mezclar respuestas de hilos. */
            for (;;) {
                sig_atomic_t esperado = 0;
                if (__atomic_compare_exchange_n(&fallo_pendiente, &esperado, 1,
                        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                    break;
                }
            }
            /* Otro hilo pudo resolver la pagina mientras esperabamos.
               No volver a solicitar una copia que ya esta disponible. */
            dsm_msi_t actual = (dsm_msi_t)pagina->estado_msi;
            if ((previo == DSM_INVALID && actual != DSM_INVALID) ||
                (previo == DSM_SHARED && actual == DSM_MODIFIED)) {
                __atomic_store_n(&fallo_pendiente, 0, __ATOMIC_RELEASE);
                return;
            }
            pagina_pendiente = (sig_atomic_t)pagina_id;
            tipo_pendiente = (sig_atomic_t)actual;
            char pedido = 'F', respuesta = 'N';
            ssize_t enviado = write(canal_pedidos[1], &pedido, 1);
            ssize_t leido = -1;
            if (enviado == 1) {
                do {
                    leido = read(canal_respuestas[0], &respuesta, 1);
                } while (leido == -1 && errno == EINTR);
            }
            __atomic_store_n(&fallo_pendiente, 0, __ATOMIC_RELEASE);
            if (leido == 1 && respuesta == 'S') {
                return;
            }
        }
    }

    /* Un acceso ajeno al DSM o sin solucion conserva el SIGSEGV normal. */
    (void)sigaction(SIGSEGV, &manejador_anterior, NULL);
    if (info == NULL || info->si_code <= 0) {
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
    if (pipe(canal_pedidos) != 0) {
        return PAGER_ERROR_SISTEMA;
    }
    if (pipe(canal_respuestas) != 0) {
        close(canal_pedidos[0]); close(canal_pedidos[1]);
        return PAGER_ERROR_SISTEMA;
    }
    if (pthread_create(&hilo_resolucion, NULL, hilo_resolver_fallos, estado) != 0) {
        close(canal_pedidos[0]); close(canal_pedidos[1]);
        close(canal_respuestas[0]); close(canal_respuestas[1]);
        return PAGER_ERROR_SISTEMA;
    }

    struct sigaction accion;
    memset(&accion, 0, sizeof(accion));
    accion.sa_sigaction = manejar_fallo_pagina;
    accion.sa_flags = SA_SIGINFO;
    sigemptyset(&accion.sa_mask);

    estado_fallos = estado;
    if (sigaction(SIGSEGV, &accion, &manejador_anterior) != 0) {
        estado_fallos = NULL;
        (void)write(canal_pedidos[1], "Q", 1);
        pthread_join(hilo_resolucion, NULL);
        close(canal_pedidos[0]); close(canal_pedidos[1]);
        close(canal_respuestas[0]); close(canal_respuestas[1]);
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
    pager_fallos_configurar_resolucion(NULL, NULL);
    (void)write(canal_pedidos[1], "Q", 1);
    pthread_join(hilo_resolucion, NULL);
    close(canal_pedidos[0]); close(canal_pedidos[1]);
    close(canal_respuestas[0]); close(canal_respuestas[1]);
    canal_pedidos[0] = canal_pedidos[1] = -1;
    canal_respuestas[0] = canal_respuestas[1] = -1;
    estado_fallos = NULL;
    return PAGER_OK;
}
