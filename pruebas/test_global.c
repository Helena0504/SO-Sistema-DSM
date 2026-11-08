#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../sistema/global/global.h"

#if UINTPTR_MAX == UINT64_MAX
/* Tamano esperado para Linux x86-64; depende de la alineacion de C. */
_Static_assert(sizeof(dsm_pagina_t) == 40, "Tamano de pagina inesperado");
_Static_assert(sizeof(dsm_nodo_t) == 88, "Tamano de nodo inesperado");
_Static_assert(sizeof(dsm_asignacion_t) == 32, "Tamano de asignacion inesperado");
_Static_assert(sizeof(dsm_estado_t) == 56, "Tamano del estado inesperado");
#endif

int main(void)
{
    assert(DSM_MEMORIA_NODO_DEFAULT / DSM_PAGINA_DEFAULT == 256);
    assert(DSM_INVALID == 0);
    assert(DSM_SHARED == 1);
    assert(DSM_MODIFIED == 2);
    assert(DSM_MAX_NODOS == 64);
    puts("Estructuras DSM: prueba correcta");
    return 0;
}
