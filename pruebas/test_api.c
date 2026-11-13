#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sistema/servicios/api/api.h"

/* Función auxiliar para crear un archivo de configuración temporal válido para las pruebas */
static void crear_archivo_configuracion(const char *ruta)
{
    FILE *f = fopen(ruta, "w");
    assert(f != NULL);
    fprintf(f, "nodoId=0\n");
    fprintf(f, "ipLocal=127.0.0.1\n");
    fprintf(f, "pageSize=4096\n");
    fprintf(f, "memoriaNodo=1048576\n"); // 1 MB
    fprintf(f, "puertoBase=28000\n");
    fprintf(f, "puertoNodos=28001\n");
    fprintf(f, "puertoHeartbeat=28002\n");
    fprintf(f, "puertoPages=28003\n");
    fprintf(f, "puertoConsulta=28004\n");
    fprintf(f, "pausaH=1000\n");
    fprintf(f, "muertoH=3000\n");
    fprintf(f, "logLevel=INFO\n");
    fprintf(f, "logArchivo=dsm_test.log\n");
    fclose(f);
}

static void probar_errores_pre_inicializacion(void)
{
    /* Funciones ejecutadas antes de sistema_init deben fallar o retornar error esperado */
    assert(sistema_alloc(100) == NULL);
    assert(sistema_last_error() == ERROR_NO_INIT);
    assert(sistema_node_id() == -1);
    assert(sistema_node_cantidad() == -1);
    assert(sistema_finalize() == ERROR_NO_INIT);
}

static void probar_ciclo_vida_y_config(void)
{
    const char *config_path = "sistema_test.conf";
    crear_archivo_configuracion(config_path);

    /* Inicialización incorrecta con ruta nula */
    assert(sistema_init(NULL) == ERROR_ARG);

    /* Inicialización correcta */
    assert(sistema_init(config_path) == OK);
    assert(sistema_last_error() == OK);

    /* Doble inicialización (debe retornar OK de forma segura según la API) */
    assert(sistema_init(config_path) == OK);

    /* Verificar identidad del nodo local */
    assert(sistema_node_id() == 0);
    assert(sistema_node_cantidad() == 1); // Solo nodo local conectado

    /* Verificar consulta de info de nodo */
    node_info_t info;
    assert(sistema_node_info(0, &info) == OK);
    assert(info.nodo_id == 0);
    assert(info.puerto_pages == 28003);

    /* Consulta de nodo inválido */
    assert(sistema_node_info(99, &info) == ERROR_ARG);

    /* Finalizar sistema */
    assert(sistema_finalize() == OK);

    /* Limpiar archivo temporal */
    remove(config_path);
}

static void probar_memoria_y_paginas(void)
{
    const char *config_path = "sistema_test.conf";
    crear_archivo_configuracion(config_path);
    assert(sistema_init(config_path) == OK);

    /* Reserva de memoria dinámicamente (ej. 5000 bytes, abarca múltiples páginas de 4KB) */
    void *ptr = sistema_alloc(5000);
    assert(ptr != NULL);

    /* Escribir en la memoria reservada para asegurar que es accesible */
    volatile char *mem = ptr;
    mem[0] = 'A';
    mem[4096] = 'B';
    assert(mem[0] == 'A');
    assert(mem[4096] == 'B');

    /* Consultar y modificar estado MSI de una página (ej. página 0 y página 1) */
    page_state_t estado_pagina;
    assert(sistema_page_get_state(0, &estado_pagina) == OK);
    assert(estado_pagina == PAGE_MODIFIED); // Reservas locales inician modificadas

    assert(sistema_page_set_state(0, PAGE_SHARED) == OK);
    assert(sistema_page_get_state(0, &estado_pagina) == OK);
    assert(estado_pagina == PAGE_SHARED);

    /* Consultar estadísticas del sistema */
    stats_t estadisticas;
    assert(sistema_stats(0, &estadisticas) == OK);
    assert(estadisticas.solicitudes_memoria == 1);
    assert(estadisticas.mem_usada >= 5000);

    /* Liberar memoria */
    assert(sistema_free(ptr) == OK);

    /* Intentar liberar puntero nulo debe dar error */
    assert(sistema_free(NULL) == ERROR_ARG);

    assert(sistema_finalize() == OK);
    remove(config_path);
}

static void probar_locks_y_utilidades(void)
{
    const char *config_path = "sistema_test.conf";
    crear_archivo_configuracion(config_path);
    assert(sistema_init(config_path) == OK);

    /* Pruebas de exclusión mutua por locks locales */
    assert(sistema_lock(5) == OK);
    assert(sistema_unlock(5) == OK);

    /* Lock fuera de rango */
    assert(sistema_lock(-1) == ERROR_ARG);
    assert(sistema_lock(300) == ERROR_ARG);

    /* Impresión de nodos y tabla de páginas (solo para verificar que no fallen) */
    sistema_print_nodes();
    sistema_print_page_table(0);

    assert(sistema_finalize() == OK);
    remove(config_path);
}

int main(void)
{
    probar_errores_pre_inicializacion();
    probar_ciclo_vida_y_config();
    probar_memoria_y_paginas();
    probar_locks_y_utilidades();

    puts("Modulo API: prueba correcta");
    return 0;
}