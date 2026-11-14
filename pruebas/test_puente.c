#define _POSIX_C_SOURCE 200809L
#include "../sistema/red/puente.h"
#include "../sistema/servicios/asignacion/asig.h"
#include "../sistema/servicios/nodos/nodos.h"
#include "../sistema/servicios/paginador/pager.h"
#include "../sistema/servicios/estadisticas/stats.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static config_t config_de_nodo(uint32_t id)
{
    config_t c;
    config_default(&c);
    c.nodo_id = id;
    strcpy(c.ip_local, "127.0.0.1");
    c.puertoNodos = (uint16_t)(28011 + 10 * id);
    c.puertoHeartbeat = (uint16_t)(28012 + 10 * id);
    c.puertoPages = (uint16_t)(28013 + 10 * id);
    c.puertoConsulta = (uint16_t)(28014 + 10 * id);
    return c;
}

static void iniciar_estado(dsm_estado_t *estado, const config_t *c, void **reserva)
{
    memset(estado, 0, sizeof(*estado));
    assert(asig_tabla_inicializar(estado, c->nodo_id, c->pageSize,
                                 c->memoriaNodo) == ASIG_OK);
    assert(nodos_tabla_inicializar(estado) == NODOS_OK);
    dsm_nodo_t local = {0};
    local.nodo_id = c->nodo_id;
    strcpy(local.ip, c->ip_local);
    local.puerto_nodos = c->puertoNodos;
    local.puerto_heartbeat = c->puertoHeartbeat;
    local.puerto_pages = c->puertoPages;
    local.memoria_total = c->memoriaNodo;
    local.memoria_libre = c->memoriaNodo;
    local.estado = DSM_NODO_CONECTADO;
    assert(nodos_registrar(estado, &local) == NODOS_OK);
    assert(asig_memoria_reservar(estado, (uint64_t)getpid(),
                                 c->pageSize, reserva) == ASIG_OK);
    assert(stats_inicializar() == STATS_OK);
}

static void terminar_estado(dsm_estado_t *estado)
{
    stats_liberar();
    nodos_tabla_liberar(estado);
    asig_tabla_liberar(estado);
}

static void avisar(int fd, char c)
{
    assert(write(fd, &c, 1) == 1);
}

static void esperar(int fd, char esperado)
{
    char recibido = 0;
    assert(read(fd, &recibido, 1) == 1);
    assert(recibido == esperado);
}

int main(void)
{
    alarm(20);
    int padre_a_hijo[2];
    int hijo_a_padre[2];
    assert(pipe(padre_a_hijo) == 0);
    assert(pipe(hijo_a_padre) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(padre_a_hijo[1]);
        close(hijo_a_padre[0]);
        config_t c = config_de_nodo(1);
        dsm_estado_t estado;
        void *memoria;
        iniciar_estado(&estado, &c, &memoria);
        assert(pager_cambiar_estado(&estado, 0, DSM_INVALID) == PAGER_OK);
        puente_t puente = {0};
        assert(puente_iniciar(&puente, &estado, &c) == PUENTE_OK);
        esperar(padre_a_hijo[0], 'R');

        assert(puente_unirse(&puente, "127.0.0.1", 28011) == PUENTE_OK);
        assert(puente_enviar_heartbeat(&puente, 0) == PUENTE_OK);
        assert(puente_pedir_pagina(&puente, 0, 0) == PUENTE_OK);
        assert(strcmp((const char *)memoria, "Datos desde nodo 0") == 0);
        dsm_msi_t msi;
        assert(pager_consultar_estado(&estado, 0, &msi) == PAGER_OK);
        assert(msi == DSM_SHARED);
        stats_snapshot_t remotas;
        assert(puente_consultar_stats(&puente, 0, 28014, &remotas) == PUENTE_OK);
        assert(remotas.contadores[STATS_PAGINA_ENVIADA] >= 1);
        avisar(hijo_a_padre[1], 'P');
        esperar(padre_a_hijo[0], 'I');
        assert(pager_consultar_estado(&estado, 0, &msi) == PAGER_OK);
        assert(msi == DSM_INVALID);
        puente_detener(&puente);
        terminar_estado(&estado);
        close(padre_a_hijo[0]);
        close(hijo_a_padre[1]);
        _exit(0);
    }
    close(padre_a_hijo[0]);
    close(hijo_a_padre[1]);
    config_t c = config_de_nodo(0);
    dsm_estado_t estado;
    void *memoria;
    iniciar_estado(&estado, &c, &memoria);
    strcpy(memoria, "Datos desde nodo 0");
    puente_t puente = {0};
    assert(puente_iniciar(&puente, &estado, &c) == PUENTE_OK);
    avisar(padre_a_hijo[1], 'R');
    esperar(hijo_a_padre[0], 'P');
    dsm_msi_t msi;
    assert(pager_consultar_estado(&estado, 0, &msi) == PAGER_OK);
    assert(msi == DSM_SHARED);
    assert(puente_invalidar_copia(&puente, 1, 0) == PUENTE_OK);
    assert(pager_cambiar_estado(&estado, 0, DSM_MODIFIED) == PAGER_OK);
    assert(pager_consultar_estado(&estado, 0, &msi) == PAGER_OK);
    assert(msi == DSM_MODIFIED);
    strcpy(memoria, "Escritura exclusiva recuperada");
    avisar(padre_a_hijo[1], 'I');
    int estado_hijo;
    assert(waitpid(pid, &estado_hijo, 0) == pid);
    assert(WIFEXITED(estado_hijo) && WEXITSTATUS(estado_hijo) == 0);
    puente_detener(&puente);
    terminar_estado(&estado);
    close(padre_a_hijo[1]);
    close(hijo_a_padre[0]);
    puts("Puente red-servicios: dos procesos y pagina TCP correcta");
    return 0;
}
