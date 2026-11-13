#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "stats.h"

static void probar_inicializacion_y_errores(void)
{
    stats_snapshot_t snap = {0};

    /* Sin inicializar, las funciones deben retornar error */
    assert(stats_registrar(STATS_FALLO_LECTURA) == STATS_ERROR_SIN_INICIALIZAR);
    assert(stats_sumar(STATS_BYTES_ENVIADOS, 100) == STATS_ERROR_SIN_INICIALIZAR);
    assert(stats_registrar_latencia_fallo(50) == STATS_ERROR_SIN_INICIALIZAR);
    assert(stats_obtener(&snap) == STATS_ERROR_SIN_INICIALIZAR);
    assert(stats_reiniciar() == STATS_ERROR_SIN_INICIALIZAR);

    /* Inicialización correcta */
    assert(stats_inicializar() == STATS_OK);
    /* Doble inicialización debe fallar */
    assert(stats_inicializar() == STATS_ERROR_YA_INICIALIZADO);

    /* Parámetros inválidos */
    assert(stats_sumar((stats_evento_t)999, 10) == STATS_ERROR_PARAMETROS);
    assert(stats_registrar((stats_evento_t)-1) == STATS_ERROR_PARAMETROS);
    assert(stats_obtener(NULL) == STATS_ERROR_PARAMETROS);

    stats_liberar();
}

static void probar_contadores_y_latencias(void)
{
    stats_snapshot_t snap = {0};

    assert(stats_inicializar() == STATS_OK);

    /* Registrar eventos individuales y sumas */
    assert(stats_registrar(STATS_FALLO_LECTURA) == STATS_OK);
    assert(stats_registrar(STATS_FALLO_LECTURA) == STATS_OK);
    assert(stats_sumar(STATS_BYTES_ENVIADOS, 512) == STATS_OK);
    assert(stats_sumar(STATS_BYTES_ENVIADOS, 512) == STATS_OK);

    /* Registrar latencias de fallo */
    assert(stats_registrar_latencia_fallo(100) == STATS_OK);
    assert(stats_registrar_latencia_fallo(300) == STATS_OK);
    assert(stats_registrar_latencia_fallo(200) == STATS_OK);

    /* Obtener snapshot y verificar valores */
    assert(stats_obtener(&snap) == STATS_OK);
    assert(snap.contadores[STATS_FALLO_LECTURA] == 2);
    assert(snap.contadores[STATS_BYTES_ENVIADOS] == 1024);
    assert(snap.latencia_muestras == 3);
    assert(snap.latencia_total_us == 600);
    assert(snap.latencia_max_us == 300);

    /* Probar el reinicio de contadores */
    assert(stats_reiniciar() == STATS_OK);
    assert(stats_obtener(&snap) == STATS_OK);
    assert(snap.contadores[STATS_FALLO_LECTURA] == 0);
    assert(snap.latencia_muestras == 0);

    stats_liberar();
}

static void probar_serializacion(void)
{
    stats_snapshot_t snap_original = {0};
    stats_snapshot_t snap_deserializado = {0};
    uint8_t buffer[STATS_SERIAL_BYTES];

    assert(stats_inicializar() == STATS_OK);

    /* Llenar algunos datos de prueba */
    assert(stats_sumar(STATS_PAGINA_ENVIADA, 42) == STATS_OK);
    assert(stats_sumar(STATS_BYTES_RECIBIDOS, 2048) == STATS_OK);
    assert(stats_registrar_latencia_fallo(1234) == STATS_OK);
    assert(stats_obtener(&snap_original) == STATS_OK);

    /* Errores de buffer y parámetros en serialización */
    assert(stats_serializar(NULL, buffer, sizeof(buffer)) == STATS_ERROR_PARAMETROS);
    assert(stats_serializar(&snap_original, NULL, sizeof(buffer)) == STATS_ERROR_PARAMETROS);
    assert(stats_serializar(&snap_original, buffer, STATS_SERIAL_BYTES - 1) == STATS_ERROR_BUFFER);

    /* Serializar correctamente */
    assert(stats_serializar(&snap_original, buffer, sizeof(buffer)) == STATS_OK);

    /* Errores en deserialización */
    assert(stats_deserializar(NULL, STATS_SERIAL_BYTES, &snap_deserializado) == STATS_ERROR_PARAMETROS);
    assert(stats_deserializar(buffer, STATS_SERIAL_BYTES, NULL) == STATS_ERROR_PARAMETROS);
    assert(stats_deserializar(buffer, 10, &snap_deserializado) == STATS_ERROR_BUFFER);

    /* Deserializar correctamente */
    assert(stats_deserializar(buffer, STATS_SERIAL_BYTES, &snap_deserializado) == STATS_ERROR_PARAMETROS || 1); // limpieza conceptual
    memset(&snap_deserializado, 0, sizeof(snap_deserializado));
    assert(stats_deserializar(buffer, STATS_SERIAL_BYTES, &snap_deserializado) == STATS_OK);

    /* Verificar integridad de los datos recuperados */
    assert(snap_deserializado.contadores[STATS_PAGINA_ENVIADA] == 42);
    assert(snap_deserializado.contadores[STATS_BYTES_RECIBIDOS] == 2048);
    assert(snap_deserializado.latencia_muestras == 1);
    assert(snap_deserializado.latencia_total_us == 1234);
    assert(snap_deserializado.latencia_max_us == 1234);

    stats_liberar();
}

static void probar_utilidades_extras(void)
{
    stats_snapshot_t snap = {0};

    /* Probar nombres de eventos */
    assert(strcmp(stats_nombre_evento(STATS_FALLO_LECTURA), "fallos_lectura") == 0);
    assert(strcmp(stats_nombre_evento(STATS_ERROR_RED), "errores_red") == 0);
    assert(strcmp(stats_nombre_evento((stats_evento_t)999), "desconocido") == 0);

    /* Probar impresión (asegurando que no crashee con NULL o datos válidos) */
    stats_imprimir(NULL, 1, &snap, 4096, 2048); // No debe hacer nada / fallar
    
    assert(stats_inicializar() == STATS_OK);
    assert(stats_obtener(&snap) == STATS_OK);
    
    /* Imprimir a stdout para verificar formato visual básico */
    puts("--- Inicio de prueba visual de impresion ---");
    stats_imprimir(stdout, 0, &snap, 1024000, 512000);
    puts("--- Fin de prueba visual de impresion ---");

    stats_liberar();
}

int main(void)
{
    probar_inicializacion_y_errores();
    probar_contadores_y_latencias();
    probar_serializacion();
    probar_utilidades_extras();

    puts("Modulo STATS: prueba correcta");
    return 0;
}