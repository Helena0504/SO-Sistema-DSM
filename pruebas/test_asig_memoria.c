#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../sistema/servicios/asignacion/asig.h"

static void probar_reservas_y_liberacion(void)
{
    dsm_estado_t estado = {0};
    void *primera = NULL;
    void *segunda = NULL;
    void *reutilizada = NULL;

    assert(asig_tabla_inicializar(&estado, 1, DSM_PAGINA_DEFAULT,
                                 DSM_MEMORIA_NODO_DEFAULT) == ASIG_OK);
    assert(estado.tabla_asignaciones != NULL);

    assert(asig_memoria_reservar(&estado, 42, 5000, &primera) == ASIG_OK);
    assert(primera != NULL);
    assert(estado.memoria_libre == DSM_MEMORIA_NODO_DEFAULT - 8192);
    assert(estado.tabla_paginas[0].ocupada == 1);
    assert(estado.tabla_paginas[1].estado_msi == DSM_MODIFIED);
    assert(estado.tabla_paginas[0].proceso_id == 42);
    assert(estado.tabla_paginas[0].nodos_con_copia == (UINT64_C(1) << 1));
    assert(estado.tabla_asignaciones[0].cantidad_paginas == 2);
    assert(estado.tabla_asignaciones[0].base_virtual == (uintptr_t)primera);

    memset(primera, 0xA5, 8192);
    assert(((unsigned char *)primera)[8191] == 0xA5);

    assert(asig_memoria_reservar(&estado, 84, 4096, &segunda) == ASIG_OK);
    assert(estado.tabla_paginas[2].proceso_id == 84);
    assert(estado.memoria_libre == DSM_MEMORIA_NODO_DEFAULT - 12288);

    assert(asig_memoria_liberar(&estado, primera) == ASIG_OK);
    assert(estado.memoria_libre == DSM_MEMORIA_NODO_DEFAULT - 4096);
    assert(estado.tabla_paginas[0].estado_msi == DSM_INVALID);
    assert(estado.tabla_paginas[1].ocupada == 0);
    assert(estado.tabla_paginas[2].ocupada == 1);

    assert(asig_memoria_reservar(&estado, 51, 4096, &reutilizada) == ASIG_OK);
    assert(estado.tabla_paginas[0].proceso_id == 51);
    assert(asig_memoria_liberar(&estado, reutilizada) == ASIG_OK);
    assert(asig_memoria_liberar(&estado, segunda) == ASIG_OK);
    assert(estado.memoria_libre == estado.memoria_total);

    asig_tabla_liberar(&estado);
    assert(estado.tabla_paginas == NULL);
    assert(estado.tabla_asignaciones == NULL);
}

static void probar_errores(void)
{
    dsm_estado_t estado = {0};
    void *direccion = (void *)(uintptr_t)0x1;
    unsigned char fuera = 0;

    assert(asig_memoria_reservar(&estado, 1, 4096, &direccion) ==
           ASIG_ERROR_PARAMETROS);
    assert(direccion == NULL);
    assert(asig_tabla_inicializar(&estado, 0, DSM_PAGINA_DEFAULT,
                                 DSM_MEMORIA_NODO_DEFAULT) == ASIG_OK);
    assert(asig_memoria_reservar(&estado, 1, 4096, NULL) ==
           ASIG_ERROR_PARAMETROS);
    assert(asig_memoria_reservar(&estado, 0, 4096, &direccion) ==
           ASIG_ERROR_PARAMETROS);
    assert(asig_memoria_reservar(&estado, 1, 0, &direccion) ==
           ASIG_ERROR_PARAMETROS);
    assert(asig_memoria_reservar(&estado, 1,
                                 (size_t)DSM_MEMORIA_NODO_DEFAULT + 1,
                                 &direccion) == ASIG_ERROR_SIN_ESPACIO);
    assert(direccion == NULL);
    assert(asig_memoria_liberar(&estado, &fuera) ==
           ASIG_ERROR_NO_ENCONTRADO);
    assert(asig_memoria_reservar(&estado, 10, 4096, &direccion) == ASIG_OK);
    assert(asig_memoria_liberar(&estado, direccion) == ASIG_OK);
    assert(asig_memoria_liberar(&estado, direccion) ==
           ASIG_ERROR_NO_ENCONTRADO);
    asig_tabla_liberar(&estado);
}

static void probar_limpieza_con_reservas_activas(void)
{
    dsm_estado_t estado = {0};
    void *direccion = NULL;

    assert(asig_tabla_inicializar(&estado, 3, 8192, 1048576) == ASIG_OK);
    assert(asig_memoria_reservar(&estado, 33, 1, &direccion) == ASIG_OK);
    assert(estado.memoria_libre == 1048576 - 8192);
    assert(((unsigned char *)direccion)[0] == 0);
    ((unsigned char *)direccion)[8191] = 7;
    asig_tabla_liberar(&estado);
    assert(estado.tabla_asignaciones == NULL);
    assert(estado.tabla_paginas == NULL);
}

int main(void)
{
    probar_reservas_y_liberacion();
    probar_errores();
    probar_limpieza_con_reservas_activas();
    puts("Reserva de memoria DSM: prueba correcta");
    return 0;
}
