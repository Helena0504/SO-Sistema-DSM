#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "../sistema/servicios/nodos/nodos.h"

static dsm_nodo_t crear_nodo(uint32_t id)
{
    dsm_nodo_t nodo = {0};
    nodo.nodo_id = id;
    snprintf(nodo.ip, sizeof(nodo.ip), "192.168.1.%u", id + 10);
    nodo.puerto_pages = 9003;
    nodo.puerto_nodos = 9001;
    nodo.puerto_heartbeat = 9002;
    nodo.memoria_total = DSM_MEMORIA_NODO_DEFAULT;
    nodo.memoria_libre = DSM_MEMORIA_NODO_DEFAULT;
    nodo.estado = DSM_NODO_CONECTADO;
    return nodo;
}

static void probar_registro_y_consultas(void)
{
    dsm_estado_t estado = {0};
    dsm_nodo_t salida = {0};
    uint32_t conectados = 0;
    dsm_nodo_t nodo0 = crear_nodo(0);
    dsm_nodo_t nodo1 = crear_nodo(1);

    assert(nodos_tabla_inicializar(&estado) == NODOS_OK);
    assert(nodos_tabla_inicializar(&estado) == NODOS_ERROR_YA_INICIALIZADO);
    assert(nodos_registrar(&estado, &nodo0) == NODOS_OK);
    assert(nodos_registrar(&estado, &nodo1) == NODOS_OK);
    assert(nodos_registrar(&estado, &nodo0) == NODOS_ERROR_DUPLICADO);
    assert(estado.nodos_conocidos == 2);
    assert(nodos_obtener(&estado, 1, &salida) == NODOS_OK);
    assert(strcmp(salida.ip, "192.168.1.11") == 0);
    assert(salida.memoria_total == DSM_MEMORIA_NODO_DEFAULT);
    assert(nodos_contar_conectados(&estado, &conectados) == NODOS_OK);
    assert(conectados == 2);

    assert(nodos_cambiar_estado(&estado, 1, DSM_NODO_DESCONECTADO) == NODOS_OK);
    assert(nodos_obtener(&estado, 1, &salida) == NODOS_OK);
    assert(salida.estado == DSM_NODO_DESCONECTADO);
    assert(nodos_contar_conectados(&estado, &conectados) == NODOS_OK);
    assert(conectados == 1);
    assert(nodos_cambiar_estado(&estado, 1, DSM_NODO_CONECTADO) == NODOS_OK);

    /* Liberar nodos no debe interferir con las futuras tablas de memoria. */
    nodos_tabla_liberar(&estado);
    assert(estado.tabla_nodos == NULL);
    assert(estado.nodos_conocidos == 0);
    nodos_tabla_liberar(&estado);
}

static void probar_errores_y_limites(void)
{
    dsm_estado_t estado = {0};
    dsm_nodo_t nodo = crear_nodo(1);
    dsm_nodo_t salida = {0};
    uint32_t cantidad = 0;

    assert(nodos_tabla_inicializar(NULL) == NODOS_ERROR_PARAMETROS);
    assert(nodos_registrar(&estado, &nodo) == NODOS_ERROR_SIN_INICIALIZAR);
    assert(nodos_obtener(&estado, 1, &salida) == NODOS_ERROR_SIN_INICIALIZAR);
    assert(nodos_contar_conectados(&estado, &cantidad) ==
           NODOS_ERROR_SIN_INICIALIZAR);
    assert(nodos_tabla_inicializar(&estado) == NODOS_OK);

    nodo.ip[0] = '\0';
    assert(nodos_registrar(&estado, &nodo) == NODOS_ERROR_PARAMETROS);
    nodo = crear_nodo(1);
    nodo.memoria_libre = nodo.memoria_total + 1;
    assert(nodos_registrar(&estado, &nodo) == NODOS_ERROR_PARAMETROS);
    nodo = crear_nodo(1);
    nodo.nodo_id = DSM_MAX_NODOS;
    assert(nodos_registrar(&estado, &nodo) == NODOS_ERROR_PARAMETROS);
    assert(nodos_obtener(&estado, 42, &salida) == NODOS_ERROR_NO_ENCONTRADO);
    assert(nodos_cambiar_estado(&estado, 42, DSM_NODO_DESCONECTADO) ==
           NODOS_ERROR_NO_ENCONTRADO);

    for (uint32_t i = 0; i < DSM_MAX_NODOS; ++i) {
        nodo = crear_nodo(i);
        assert(nodos_registrar(&estado, &nodo) == NODOS_OK);
    }
    assert(estado.nodos_conocidos == DSM_MAX_NODOS);
    assert(nodos_contar_conectados(&estado, &cantidad) == NODOS_OK);
    assert(cantidad == DSM_MAX_NODOS);
    nodos_tabla_liberar(&estado);
}

typedef struct {
    dsm_estado_t *estado;
    uint32_t inicio;
} lote_t;

static void *registrar_lote(void *argumento)
{
    lote_t *lote = argumento;
    for (uint32_t i = lote->inicio; i < lote->inicio + 16; ++i) {
        dsm_nodo_t nodo = crear_nodo(i);
        assert(nodos_registrar(lote->estado, &nodo) == NODOS_OK);
    }
    return NULL;
}

static void probar_registro_concurrente(void)
{
    dsm_estado_t estado = {0};
    pthread_t hilos[4];
    lote_t lotes[4];
    uint32_t conectados = 0;

    assert(nodos_tabla_inicializar(&estado) == NODOS_OK);
    for (uint32_t i = 0; i < 4; ++i) {
        lotes[i] = (lote_t){.estado = &estado, .inicio = i * 16};
        assert(pthread_create(&hilos[i], NULL, registrar_lote, &lotes[i]) == 0);
    }
    for (uint32_t i = 0; i < 4; ++i) {
        assert(pthread_join(hilos[i], NULL) == 0);
    }
    assert(estado.nodos_conocidos == DSM_MAX_NODOS);
    assert(nodos_contar_conectados(&estado, &conectados) == NODOS_OK);
    assert(conectados == DSM_MAX_NODOS);
    nodos_tabla_liberar(&estado);
}

int main(void)
{
    probar_registro_y_consultas();
    probar_errores_y_limites();
    probar_registro_concurrente();
    puts("Registro de nodos DSM: prueba correcta");
    return 0;
}
