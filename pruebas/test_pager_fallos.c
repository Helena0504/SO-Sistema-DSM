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

/* Ejecutamos los accesos no recuperables en hijos para conservar la prueba. */
static void esperar_segv(volatile unsigned char *direccion, int escritura)
{
    pid_t hijo = fork();
    assert(hijo >= 0);
    if (hijo == 0) {
        struct rlimit limite = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &limite);
        if (escritura) {
            *direccion = 99;
        } else {
            volatile unsigned char leido = *direccion;
            (void)leido;
        }
        _exit(0);
    }

    int resultado = 0;
    assert(waitpid(hijo, &resultado, 0) == hijo);
    assert(WIFSIGNALED(resultado));
    assert(WTERMSIG(resultado) == SIGSEGV);
}

/* Un SIGSEGV externo tampoco debe quedar oculto por nuestro manejador. */
static void esperar_senal_propagada(void)
{
    pid_t hijo = fork();
    assert(hijo >= 0);
    if (hijo == 0) {
        struct rlimit limite = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &limite);
        assert(raise(SIGSEGV) == 0);
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

    void *region = NULL;
    assert(asig_memoria_reservar(&estado, 7001, 12288, &region) == ASIG_OK);
    volatile unsigned char *datos = region;
    datos[0] = 10;
    datos[4096] = 20;
    datos[8192] = 30;

    assert(pager_fallos_instalar(NULL) == PAGER_ERROR_PARAMETROS);
    assert(pager_fallos_desinstalar() == PAGER_ERROR_NO_INSTALADO);
    assert(pager_fallos_instalar(&estado) == PAGER_OK);
    assert(pager_fallos_instalar(&estado) == PAGER_ERROR_YA_INSTALADO);
    esperar_senal_propagada();

    /* Escritura en SHARED sin copias remotas: SIGSEGV habilita escritura. */
    assert(pager_cambiar_estado(&estado, 0, DSM_SHARED) == PAGER_OK);
    assert(datos[0] == 10);
    datos[0] = 42;
    assert(datos[0] == 42);
    dsm_msi_t msi;
    assert(pager_consultar_estado(&estado, 0, &msi) == PAGER_OK);
    assert(msi == DSM_MODIFIED);
    assert(estado.tabla_paginas[0].nodo_exclusivo == 1);

    /* Con otra copia registrada no puede tomar exclusividad local. */
    assert(pager_cambiar_estado(&estado, 1, DSM_SHARED) == PAGER_OK);
    estado.tabla_paginas[1].nodos_con_copia |= UINT64_C(1) << 2;
    esperar_segv(datos + 4096, 1);
    assert(pager_consultar_estado(&estado, 1, &msi) == PAGER_OK);
    assert(msi == DSM_SHARED);
    assert(datos[4096] == 20);

    /* La pagina INVALID sigue requiriendo recuperacion de datos. */
    assert(pager_cambiar_estado(&estado, 2, DSM_INVALID) == PAGER_OK);
    esperar_segv(datos + 8192, 0);

    /* Al desinstalarlo, se recupera el comportamiento normal de Linux. */
    assert(pager_fallos_desinstalar() == PAGER_OK);
    assert(pager_fallos_desinstalar() == PAGER_ERROR_NO_INSTALADO);
    esperar_segv(datos + 4096, 1);

    /* El manejador puede instalarse otra vez en el mismo proceso. */
    assert(pager_fallos_instalar(&estado) == PAGER_OK);
    estado.tabla_paginas[1].nodos_con_copia &= ~(UINT64_C(1) << 2);
    datos[4096] = 77;
    assert(datos[4096] == 77);
    assert(pager_consultar_estado(&estado, 1, &msi) == PAGER_OK);
    assert(msi == DSM_MODIFIED);
    assert(pager_fallos_desinstalar() == PAGER_OK);

    assert(asig_memoria_liberar(&estado, region) == ASIG_OK);
    asig_tabla_liberar(&estado);
    puts("Fallos de pagina SIGSEGV: prueba correcta");
    return 0;
}
