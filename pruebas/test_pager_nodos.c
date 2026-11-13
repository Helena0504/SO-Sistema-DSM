#define _POSIX_C_SOURCE 200809L
#include "../sistema/servicios/asignacion/asig.h"
#include "../sistema/servicios/nodos/nodos.h"
#include "../sistema/servicios/paginador/pager.h"

#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static dsm_nodo_t nodo_de_prueba(uint32_t id, dsm_estado_nodo_t estado)
{
    dsm_nodo_t nodo = {0};
    nodo.nodo_id = id;
    snprintf(nodo.ip, sizeof(nodo.ip), "127.0.0.%u", id + 1);
    nodo.puerto_pages = 9003;
    nodo.puerto_nodos = 9001;
    nodo.puerto_heartbeat = 9002;
    nodo.memoria_total = DSM_MEMORIA_NODO_DEFAULT;
    nodo.memoria_libre = nodo.memoria_total;
    nodo.estado = (uint8_t)estado;
    return nodo;
}

/* Una copia remota activa debe impedir que SIGSEGV habilite escritura. */
static void esperar_escritura_protegida(volatile unsigned char *direccion)
{
    pid_t hijo = fork();
    assert(hijo >= 0);
    if (hijo == 0) {
        struct rlimit limite = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &limite);
        *direccion = 77;
        _exit(0);
    }
    int resultado = 0;
    assert(waitpid(hijo, &resultado, 0) == hijo);
    assert(WIFSIGNALED(resultado));
    assert(WTERMSIG(resultado) == SIGSEGV);
}

int main(void)
{
    dsm_estado_t estado = {0};
    assert(asig_tabla_inicializar(&estado, 1, 4096, 1048576) == ASIG_OK);
    assert(nodos_tabla_inicializar(&estado) == NODOS_OK);

    dsm_nodo_t destino = nodo_de_prueba(2, DSM_NODO_CONECTADO);
    dsm_nodo_t desconectado = nodo_de_prueba(3, DSM_NODO_DESCONECTADO);
    dsm_nodo_t ultimo = nodo_de_prueba(63, DSM_NODO_CONECTADO);
    assert(nodos_registrar(&estado, &destino) == NODOS_OK);
    assert(nodos_registrar(&estado, &desconectado) == NODOS_OK);
    assert(nodos_registrar(&estado, &ultimo) == NODOS_OK);

    void *region = NULL;
    assert(asig_memoria_reservar(&estado, 8001, 8192, &region) == ASIG_OK);
    volatile unsigned char *datos = region;
    datos[0] = 42;
    datos[4095] = 19;
    datos[4096] = 8;
    unsigned char buffer[4096] = {0};

    /* Preparar el envio no debe inventar una copia remota. */
    assert(pager_preparar_envio(&estado, 0, 2, buffer, sizeof(buffer)) == PAGER_OK);
    assert(buffer[0] == 42 && buffer[4095] == 19);
    assert(estado.tabla_paginas[0].estado_msi == DSM_MODIFIED);
    assert(estado.tabla_paginas[0].nodos_con_copia == (UINT64_C(1) << 1));

    assert(pager_preparar_envio(&estado, 0, 2, buffer, 100) ==
           PAGER_ERROR_BUFFER_PEQUENO);
    assert(pager_preparar_envio(&estado, 0, 3, buffer, sizeof(buffer)) ==
           PAGER_ERROR_NODO_NO_DISPONIBLE);
    assert(pager_preparar_envio(&estado, 0, 4, buffer, sizeof(buffer)) ==
           PAGER_ERROR_NODO_NO_DISPONIBLE);
    assert(pager_preparar_envio(&estado, 0, 1, buffer, sizeof(buffer)) ==
           PAGER_ERROR_PARAMETROS);
    assert(pager_preparar_envio(&estado, 0, 2, NULL, sizeof(buffer)) ==
           PAGER_ERROR_PARAMETROS);
    assert(pager_preparar_envio(&estado, 2, 2, buffer, sizeof(buffer)) ==
           PAGER_ERROR_NO_ASIGNADA);
    assert(pager_confirmar_copia(&estado, 0, 3) ==
           PAGER_ERROR_NODO_NO_DISPONIBLE);

    /* Esta llamada representa una confirmacion recibida por la red. */
    assert(pager_confirmar_copia(&estado, 0, 2) == PAGER_OK);
    assert(estado.tabla_paginas[0].estado_msi == DSM_SHARED);
    assert(estado.tabla_paginas[0].nodo_exclusivo == DSM_NODO_SIN_EXCLUSIVIDAD);
    assert(estado.tabla_paginas[0].nodos_con_copia ==
           ((UINT64_C(1) << 1) | (UINT64_C(1) << 2)));
    assert(datos[0] == 42);
    assert(pager_fallos_instalar(&estado) == PAGER_OK);
    esperar_escritura_protegida(datos);
    assert(pager_fallos_desinstalar() == PAGER_OK);

    assert(pager_cambiar_estado(&estado, 0, DSM_MODIFIED) ==
           PAGER_ERROR_COPIAS_REMOTAS);
    assert(pager_confirmar_invalidacion(&estado, 0, 63) ==
           PAGER_ERROR_COPIA_AUSENTE);
    assert(pager_confirmar_invalidacion(&estado, 0, 2) == PAGER_OK);
    assert(pager_confirmar_invalidacion(&estado, 0, 2) ==
           PAGER_ERROR_COPIA_AUSENTE);
    assert(pager_cambiar_estado(&estado, 0, DSM_MODIFIED) == PAGER_OK);
    datos[0] = 99;
    assert(datos[0] == 99);

    /* Un nodo con ID 63 tambien se representa sin perder bits. */
    assert(pager_preparar_envio(&estado, 1, 63, buffer, sizeof(buffer)) == PAGER_OK);
    assert(buffer[0] == 8);
    assert(pager_confirmar_copia(&estado, 1, 63) == PAGER_OK);
    assert((estado.tabla_paginas[1].nodos_con_copia & (UINT64_C(1) << 63)) != 0);
    assert(nodos_cambiar_estado(&estado, 63, DSM_NODO_DESCONECTADO) == NODOS_OK);
    assert(pager_confirmar_invalidacion(&estado, 1, 63) ==
           PAGER_ERROR_NODO_NO_DISPONIBLE);
    assert(nodos_cambiar_estado(&estado, 63, DSM_NODO_CONECTADO) == NODOS_OK);
    assert(pager_confirmar_invalidacion(&estado, 1, 63) == PAGER_OK);

    assert(pager_cambiar_estado(&estado, 1, DSM_INVALID) == PAGER_OK);
    assert(pager_preparar_envio(&estado, 1, 2, buffer, sizeof(buffer)) ==
           PAGER_ERROR_SIN_DATOS);
    assert(pager_confirmar_copia(&estado, 1, 2) == PAGER_ERROR_SIN_DATOS);
    assert(asig_memoria_liberar(&estado, region) == ASIG_OK);
    nodos_tabla_liberar(&estado);
    asig_tabla_liberar(&estado);
    puts("Integracion paginador y nodos: prueba correcta");
    return 0;
}
