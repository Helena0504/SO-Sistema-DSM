#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../sistema/servicios/asignacion/asig.h"

static void probar_tabla_inicial(void)
{
    dsm_estado_t estado = {0};

    assert(asig_tabla_inicializar(&estado, 2, DSM_PAGINA_DEFAULT,
                                 DSM_MEMORIA_NODO_DEFAULT) == ASIG_OK);
    assert(estado.nodo_id == 2);
    assert(estado.tamano_pagina == 4096);
    assert(estado.numero_paginas == 256);
    assert(estado.memoria_total == 1048576);
    assert(estado.memoria_libre == estado.memoria_total);
    assert(estado.tabla_paginas != NULL);

    for (uint32_t i = 0; i < estado.numero_paginas; ++i) {
        const dsm_pagina_t *pagina = &estado.tabla_paginas[i];
        assert(pagina->pagina_id == i);
        assert(pagina->nodo_origen == 2);
        assert(pagina->nodo_exclusivo == DSM_NODO_SIN_EXCLUSIVIDAD);
        assert(pagina->estado_msi == DSM_INVALID);
        assert(pagina->ocupada == 0);
        assert(pagina->nodos_con_copia == 0);
    }

    assert(asig_tabla_inicializar(&estado, 2, DSM_PAGINA_DEFAULT,
                                 DSM_MEMORIA_NODO_DEFAULT) ==
           ASIG_ERROR_INICIALIZADO);

    asig_tabla_liberar(&estado);
    assert(estado.tabla_paginas == NULL);
    assert(estado.numero_paginas == 0);
    asig_tabla_liberar(&estado);
}

static void probar_parametros(void)
{
    dsm_estado_t estado = {0};
    assert(asig_tabla_inicializar(NULL, 0, 4096, 4096) ==
           ASIG_ERROR_PARAMETROS);
    assert(asig_tabla_inicializar(&estado, DSM_MAX_NODOS, 4096, 4096) ==
           ASIG_ERROR_PARAMETROS);
    assert(asig_tabla_inicializar(&estado, 0, 0, 4096) ==
           ASIG_ERROR_PARAMETROS);
    assert(asig_tabla_inicializar(&estado, 0, 4096, 0) ==
           ASIG_ERROR_PARAMETROS);
    assert(asig_tabla_inicializar(&estado, 0, 4096, 5000) ==
           ASIG_ERROR_PARAMETROS);
    assert(asig_tabla_inicializar(&estado, 0, 4096,
                                 ((uint64_t)UINT32_MAX + 1) * 4096) ==
           ASIG_ERROR_PARAMETROS);
    assert(estado.tabla_paginas == NULL);

    assert(asig_tabla_inicializar(&estado, 0, 8192, 1048576) == ASIG_OK);
    assert(estado.numero_paginas == 128);
    asig_tabla_liberar(&estado);
}

int main(void)
{
    probar_tabla_inicial();
    probar_parametros();
    puts("Tabla de paginas DSM: prueba correcta");
    return 0;
}
