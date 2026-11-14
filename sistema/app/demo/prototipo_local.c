#include "../../servicios/asignacion/asig.h"
#include "../../servicios/nodos/nodos.h"
#include "../../servicios/paginador/pager.h"

#include <stdint.h>
#include <stdio.h>

static dsm_nodo_t crear_remoto(void)
{
    dsm_nodo_t remoto = {0};
    remoto.nodo_id = 2;
    snprintf(remoto.ip, sizeof(remoto.ip), "127.0.0.3");
    remoto.puerto_nodos = 9001;
    remoto.puerto_heartbeat = 9002;
    remoto.puerto_pages = 9003;
    remoto.memoria_total = DSM_MEMORIA_NODO_DEFAULT;
    remoto.memoria_libre = remoto.memoria_total;
    remoto.estado = DSM_NODO_CONECTADO;
    return remoto;
}

int main(void)
{
    dsm_estado_t estado = {0};
    dsm_nodo_t remoto = crear_remoto();
    void *region = NULL;
    unsigned char buffer[DSM_PAGINA_DEFAULT] = {0};
    int resultado = 1;

    if (asig_tabla_inicializar(&estado, 1, DSM_PAGINA_DEFAULT,
                              DSM_MEMORIA_NODO_DEFAULT) != ASIG_OK) {
        fprintf(stderr, "No se pudo inicializar la memoria.\n");
        return 1;
    }
    if (nodos_tabla_inicializar(&estado) != NODOS_OK ||
        nodos_registrar(&estado, &remoto) != NODOS_OK) {
        fprintf(stderr, "No se pudo registrar el nodo remoto.\n");
        goto salir;
    }
    if (asig_memoria_reservar(&estado, 9001, 4096, &region) != ASIG_OK) {
        fprintf(stderr, "No se pudo reservar una pagina.\n");
        goto salir;
    }

    unsigned char *datos = region;
    datos[0] = 27;
    datos[1] = 54;
    printf("Nodo local 1: pagina 0 reservada, datos: %u, %u\n",
           datos[0], datos[1]);

    if (pager_preparar_envio(&estado, 0, 2, buffer, sizeof(buffer)) != PAGER_OK) {
        fprintf(stderr, "No se pudo preparar el envio.\n");
        goto salir;
    }
    printf("Buffer de envio preparado para el nodo 2: %u, %u\n",
           buffer[0], buffer[1]);

    /* El acuse de recibo se simula. No hay conexion TCP en esta demo. */
    if (pager_confirmar_copia(&estado, 0, 2) != PAGER_OK) {
        fprintf(stderr, "No se pudo confirmar la copia.\n");
        goto salir;
    }
    printf("Pagina 0: SHARED; copias registradas en los nodos 1 y 2\n");

    if (pager_confirmar_invalidacion(&estado, 0, 2) != PAGER_OK ||
        pager_cambiar_estado(&estado, 0, DSM_MODIFIED) != PAGER_OK) {
        fprintf(stderr, "No se pudo recuperar la escritura exclusiva.\n");
        goto salir;
    }
    datos[0] = 81;
    printf("Copia remota invalidada; pagina 0: MODIFIED; nuevo dato: %u\n",
           datos[0]);
    resultado = 0;

salir:
    if (region != NULL &&
        asig_memoria_liberar(&estado, region) != ASIG_OK) {
        fprintf(stderr, "No se pudo liberar la region de memoria.\n");
        resultado = 1;
    }
    nodos_tabla_liberar(&estado);
    asig_tabla_liberar(&estado);
    return resultado;
}
