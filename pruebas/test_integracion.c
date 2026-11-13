#define _POSIX_C_SOURCE 200809L
#include "../sistema/servicios/asignacion/asig.h"
#include "../sistema/servicios/nodos/nodos.h"
#include "../sistema/servicios/paginador/pager.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static dsm_nodo_t crear_nodo(uint32_t id)
{
    dsm_nodo_t nodo = {0};
    nodo.nodo_id = id;
    snprintf(nodo.ip, sizeof(nodo.ip), "127.0.0.%u", id + 1);
    nodo.puerto_nodos = 9001;
    nodo.puerto_heartbeat = 9002;
    nodo.puerto_pages = 9003;
    nodo.memoria_total = DSM_MEMORIA_NODO_DEFAULT;
    nodo.memoria_libre = nodo.memoria_total;
    nodo.estado = DSM_NODO_CONECTADO;
    return nodo;
}

int main(void)
{
    dsm_estado_t estado = {0};
    dsm_nodo_t local = crear_nodo(1);
    dsm_nodo_t remoto = crear_nodo(2);
    void *region = NULL;
    unsigned char buffer[DSM_PAGINA_DEFAULT] = {0};
    uint64_t ahora = 0;
    uint32_t caidos = 0;
    uint32_t conectados = 0;
    dsm_msi_t msi;

    assert(asig_tabla_inicializar(&estado, 1, DSM_PAGINA_DEFAULT,
                                 DSM_MEMORIA_NODO_DEFAULT) == ASIG_OK);
    assert(nodos_tabla_inicializar(&estado) == NODOS_OK);
    assert(nodos_registrar(&estado, &local) == NODOS_OK);
    assert(nodos_registrar(&estado, &remoto) == NODOS_OK);
    assert(nodos_contar_conectados(&estado, &conectados) == NODOS_OK);
    assert(conectados == 2);

    /* Dos paginas locales para el proceso de prueba. */
    assert(asig_memoria_reservar(&estado, 5001, 6000, &region) == ASIG_OK);
    assert(estado.memoria_libre == DSM_MEMORIA_NODO_DEFAULT - 8192);
    volatile unsigned char *datos = region;
    datos[0] = 21;
    datos[4095] = 45;
    datos[4096] = 78;

    /* La red aun no envia datos: copiamos el buffer y simulamos el acuse. */
    assert(pager_preparar_envio(&estado, 0, 2, buffer,
                               sizeof(buffer)) == PAGER_OK);
    assert(buffer[0] == 21 && buffer[4095] == 45);
    assert(pager_confirmar_copia(&estado, 0, 2) == PAGER_OK);
    assert(pager_consultar_estado(&estado, 0, &msi) == PAGER_OK);
    assert(msi == DSM_SHARED);
    assert(pager_cambiar_estado(&estado, 0, DSM_MODIFIED) ==
           PAGER_ERROR_COPIAS_REMOTAS);

    assert(pager_confirmar_invalidacion(&estado, 0, 2) == PAGER_OK);
    assert(pager_fallos_instalar(&estado) == PAGER_OK);
    /* La escritura en SHARED activa SIGSEGV y recupera acceso local. */
    datos[0] = 99;
    assert(pager_consultar_estado(&estado, 0, &msi) == PAGER_OK);
    assert(msi == DSM_MODIFIED && datos[0] == 99);
    assert(pager_fallos_desinstalar() == PAGER_OK);

    /* Al vencer el heartbeat, el destino ya no puede recibir paginas. */
    assert(nodos_tiempo_ms(&ahora) == NODOS_OK);
    assert(nodos_heartbeat_recibido(&estado, 2, ahora) == NODOS_OK);
    assert(nodos_verificar_inactivos(&estado, ahora + 3000,
                                    NODOS_MUERTO_H_DEFAULT, &caidos) == NODOS_OK);
    assert(caidos == 1);
    assert(pager_preparar_envio(&estado, 1, 2, buffer, sizeof(buffer)) ==
           PAGER_ERROR_NODO_NO_DISPONIBLE);

    assert(asig_memoria_liberar(&estado, region) == ASIG_OK);
    assert(estado.memoria_libre == estado.memoria_total);
    nodos_tabla_liberar(&estado);
    asig_tabla_liberar(&estado);
    puts("Integracion del prototipo DSM: prueba correcta");
    return 0;
}
