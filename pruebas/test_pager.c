#define _POSIX_C_SOURCE 200809L
#include "../sistema/servicios/asignacion/asig.h"
#include "../sistema/servicios/paginador/pager.h"

#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Verifica en un proceso hijo que Linux realmente rechace el acceso. */
static void esperar_segv(volatile unsigned char *direccion, int escritura)
{
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        struct rlimit limite = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &limite);
        if (escritura) {
            *direccion = 42;
        } else {
            volatile unsigned char valor = *direccion;
            (void)valor;
        }
        _exit(0);
    }

    int estado = 0;
    assert(waitpid(pid, &estado, 0) == pid);
    assert(WIFSIGNALED(estado));
    assert(WTERMSIG(estado) == SIGSEGV);
}

int main(void)
{
    dsm_estado_t estado = {0};
    assert(asig_tabla_inicializar(&estado, 1, 4096, 1048576) == ASIG_OK);

    void *primera = NULL;
    void *segunda = NULL;
    assert(asig_memoria_reservar(&estado, 1001, 5000, &primera) == ASIG_OK);
    assert(asig_memoria_reservar(&estado, 1002, 4096, &segunda) == ASIG_OK);
    volatile unsigned char *a = primera;
    volatile unsigned char *b = segunda;
    a[0] = 5;
    a[4096] = 7;
    b[0] = 9;

    dsm_msi_t msi = DSM_INVALID;
    assert(pager_consultar_estado(&estado, 0, &msi) == PAGER_OK);
    assert(msi == DSM_MODIFIED);

    // Una pagina compartida puede leerse, pero no escribirse.
    assert(pager_cambiar_estado(&estado, 1, DSM_SHARED) == PAGER_OK);
    assert(a[4096] == 7);
    esperar_segv(a + 4096, 1);
    assert(pager_consultar_estado(&estado, 1, &msi) == PAGER_OK);
    assert(msi == DSM_SHARED);

    // Sin copias remotas, puede recuperar escritura exclusiva.
    assert(pager_cambiar_estado(&estado, 1, DSM_MODIFIED) == PAGER_OK);
    a[4096] = 11;
    assert(a[4096] == 11);

    // No se concede exclusividad si otra maquina tiene una copia.
    estado.tabla_paginas[1].nodos_con_copia |= UINT64_C(1) << 2;
    assert(pager_cambiar_estado(&estado, 1, DSM_SHARED) == PAGER_OK);
    assert(pager_cambiar_estado(&estado, 1, DSM_MODIFIED) ==
           PAGER_ERROR_COPIAS_REMOTAS);
    assert(estado.tabla_paginas[1].estado_msi == DSM_SHARED);

    // Invalid impide lectura; falta recuperar la pagina para habilitarla.
    assert(pager_cambiar_estado(&estado, 2, DSM_INVALID) == PAGER_OK);
    esperar_segv(b, 0);
    assert(pager_cambiar_estado(&estado, 2, DSM_MODIFIED) ==
           PAGER_ERROR_SIN_DATOS);
    assert(pager_cambiar_estado(&estado, 2, DSM_SHARED) ==
           PAGER_ERROR_SIN_DATOS);

    assert(pager_cambiar_estado(&estado, 42, DSM_SHARED) == PAGER_ERROR_NO_ASIGNADA);
    assert(pager_cambiar_estado(&estado, 1, (dsm_msi_t)99) ==
           PAGER_ERROR_PARAMETROS);
    assert(asig_memoria_liberar(&estado, primera) == ASIG_OK);
    assert(asig_memoria_liberar(&estado, segunda) == ASIG_OK);
    asig_tabla_liberar(&estado);
    puts("Permisos MSI del paginador: prueba correcta");
    return 0;
}
