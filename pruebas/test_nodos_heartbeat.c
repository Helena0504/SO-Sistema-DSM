#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../sistema/servicios/nodos/nodos.h"

static dsm_nodo_t nodo_de_prueba(uint32_t id)
{
    dsm_nodo_t nodo = {0};
    nodo.nodo_id = id;
    snprintf(nodo.ip, sizeof(nodo.ip), "127.0.0.%u", id + 1);
    nodo.puerto_pages = 9003;
    nodo.puerto_nodos = 9001;
    nodo.puerto_heartbeat = 9002;
    nodo.memoria_total = DSM_MEMORIA_NODO_DEFAULT;
    nodo.memoria_libre = nodo.memoria_total;
    nodo.estado = DSM_NODO_CONECTADO;
    return nodo;
}

static void probar_heartbeat(void)
{
    dsm_estado_t estado = {0};
    dsm_nodo_t local = nodo_de_prueba(0);
    dsm_nodo_t remoto = nodo_de_prueba(1);
    dsm_nodo_t copia = {0};
    uint32_t caidos = 123;
    uint64_t inicio = 0;

    assert(nodos_tiempo_ms(&inicio) == NODOS_OK);
    assert(nodos_tabla_inicializar(&estado) == NODOS_OK);
    assert(nodos_registrar(&estado, &local) == NODOS_OK);
    assert(nodos_registrar(&estado, &remoto) == NODOS_OK);
    estado.nodo_id = 0;

    /* Se usa el reloj monotónico para simular pasos de tiempo sin dormir. */
    assert(nodos_heartbeat_recibido(&estado, 1, inicio + 1000) == NODOS_OK);
    assert(nodos_verificar_inactivos(&estado, inicio + 3999,
                                    NODOS_MUERTO_H_DEFAULT, &caidos) == NODOS_OK);
    assert(caidos == 0);
    assert(nodos_verificar_inactivos(&estado, inicio + 4000,
                                    NODOS_MUERTO_H_DEFAULT, &caidos) == NODOS_OK);
    assert(caidos == 1);
    assert(nodos_obtener(&estado, 1, &copia) == NODOS_OK);
    assert(copia.estado == DSM_NODO_DESCONECTADO);

    /* Revisar dos veces no cuenta la misma caida de nuevo. */
    assert(nodos_verificar_inactivos(&estado, inicio + 8000,
                                    NODOS_MUERTO_H_DEFAULT, &caidos) == NODOS_OK);
    assert(caidos == 0);

    /* Al regresar, primero recibe el heartbeat y sigue fuera de servicio. */
    assert(nodos_heartbeat_recibido(&estado, 1, inicio + 8100) == NODOS_OK);
    assert(nodos_obtener(&estado, 1, &copia) == NODOS_OK);
    assert(copia.estado == DSM_NODO_DESCONECTADO);
    assert(copia.ultimo_heartbeat_ms == inicio + 8100);
    /* Esta llamada representa la resinc. realizada por nodos/red. */
    assert(nodos_cambiar_estado(&estado, 1, DSM_NODO_CONECTADO) == NODOS_OK);
    assert(nodos_heartbeat_recibido(&estado, 1, inicio + 8000) ==
           NODOS_ERROR_PARAMETROS);
    assert(nodos_obtener(&estado, 1, &copia) == NODOS_OK);
    assert(copia.ultimo_heartbeat_ms == inicio + 8100);

    /* El nodo local no se declara caido por no enviarse mensajes a si mismo. */
    assert(nodos_verificar_inactivos(&estado, inicio + 100000,
                                    NODOS_MUERTO_H_DEFAULT, &caidos) == NODOS_OK);
    assert(caidos == 1);
    assert(nodos_obtener(&estado, 0, &copia) == NODOS_OK);
    assert(copia.estado == DSM_NODO_CONECTADO);
    nodos_tabla_liberar(&estado);
}

static void probar_intervalos_y_errores(void)
{
    dsm_estado_t estado = {0};
    uint32_t caidos = 0;
    uint64_t ahora = 0;

    assert(nodos_tiempo_ms(NULL) == NODOS_ERROR_PARAMETROS);
    assert(nodos_tiempo_ms(&ahora) == NODOS_OK);
    assert(nodos_debe_enviar_heartbeat(0, 1000, 1000) == 1);
    assert(nodos_debe_enviar_heartbeat(1000, 1999, 1000) == 0);
    assert(nodos_debe_enviar_heartbeat(1000, 2000, 1000) == 1);
    assert(nodos_debe_enviar_heartbeat(2000, 1500, 1000) == 0);
    assert(nodos_debe_enviar_heartbeat(1000, 2000, 0) == 0);

    assert(nodos_heartbeat_recibido(NULL, 0, ahora) == NODOS_ERROR_PARAMETROS);
    assert(nodos_heartbeat_recibido(&estado, 1, ahora) ==
           NODOS_ERROR_SIN_INICIALIZAR);
    assert(nodos_verificar_inactivos(&estado, ahora, 3000, &caidos) ==
           NODOS_ERROR_SIN_INICIALIZAR);
    assert(nodos_tabla_inicializar(&estado) == NODOS_OK);
    assert(nodos_heartbeat_recibido(&estado, 10, ahora) ==
           NODOS_ERROR_NO_ENCONTRADO);
    assert(nodos_verificar_inactivos(&estado, ahora, 0, &caidos) ==
           NODOS_ERROR_PARAMETROS);
    assert(nodos_verificar_inactivos(&estado, ahora, 3000, NULL) ==
           NODOS_ERROR_PARAMETROS);
    nodos_tabla_liberar(&estado);
}

int main(void)
{
    probar_heartbeat();
    probar_intervalos_y_errores();
    puts("Heartbeat DSM: prueba correcta");
    return 0;
}
