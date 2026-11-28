#define _POSIX_C_SOURCE 200809L
#include "../sistema/api/api.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int al_hijo[2], al_padre[2];

static void enviar(int fd, char c)
{
    assert(write(fd, &c, 1) == 1);
}

static void esperar(int fd, char esperado)
{
    char c;
    assert(read(fd, &c, 1) == 1 && c == esperado);
}

static void crear_conf(char *ruta, size_t cap, pid_t padre, unsigned id)
{
    snprintf(ruta, cap, "/tmp/dsm_consultas_%ld_%u.conf", (long)padre, id);
    FILE *f = fopen(ruta, "w");
    assert(f != NULL);
    fprintf(f, "nodoId=%u\nipLocal=127.0.0.1\n", id);
    fprintf(f, "puertoBase=%u\npuertoNodos=%u\n"
               "puertoHeartbeat=%u\npuertoPages=%u\npuertoConsulta=%u\n",
            28030 + id * 10, 28031 + id * 10,
            28032 + id * 10, 28033 + id * 10, 28034 + id * 10);
    fprintf(f, "logArchivo=/tmp/dsm_consultas_%ld_%u.log\n",
            (long)padre, id);
    assert(fclose(f) == 0);
}

static void verificar_tabla(int nodo)
{
    page_info_t *paginas = NULL;
    size_t cantidad = 0;
    assert(sistema_consultar_paginas(nodo, &paginas, &cantidad) == OK);
    assert(paginas != NULL);
    assert(cantidad == 1);
    assert(paginas[0].pagina_id == 0);
    assert(paginas[0].proceso_id != 0);
    assert(paginas[0].nodo_respaldo == (uint32_t)nodo);
    assert(paginas[0].estado_msi == PAGE_MODIFIED);
    free(paginas);
}

static void hijo(pid_t padre)
{
    close(al_hijo[1]); close(al_padre[0]);
    char ruta[128]; crear_conf(ruta, sizeof(ruta), padre, 1);
    assert(sistema_init(ruta) == OK);
    char *pagina = sistema_alloc(4096);
    assert(pagina != NULL);
    pagina[0] = 42;
    assert(sistema_red_iniciar() == OK);
    enviar(al_padre[1], 'R');
    esperar(al_hijo[0], 'I');
    assert(sistema_red_unirse("127.0.0.1", 28031) == OK);
    enviar(al_padre[1], 'J');
    esperar(al_hijo[0], 'Q');
    verificar_tabla(0);
    stats_t s;
    assert(sistema_stats(0, &s) == OK);
    assert(s.mem_usada == 4096);
    enviar(al_padre[1], 'D');
    esperar(al_hijo[0], 'X');
    assert(sistema_finalize() == OK);
    remove(ruta);
    _exit(0);
}

int main(void)
{
    alarm(35);
    assert(pipe(al_hijo) == 0);
    assert(pipe(al_padre) == 0);
    pid_t padre = getpid();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) hijo(padre);
    close(al_hijo[0]); close(al_padre[1]);
    esperar(al_padre[0], 'R');

    char ruta[128]; crear_conf(ruta, sizeof(ruta), padre, 0);
    assert(sistema_init(ruta) == OK);
    char *pagina = sistema_alloc(4096);
    assert(pagina != NULL);
    pagina[0] = 7;
    assert(sistema_red_iniciar() == OK);
    enviar(al_hijo[1], 'I');
    esperar(al_padre[0], 'J');
    assert(sistema_node_cantidad() == 2);

    verificar_tabla(0);
    verificar_tabla(1);
    stats_t remotas;
    assert(sistema_stats(1, &remotas) == OK);
    assert(remotas.mem_usada == 4096);
    assert(remotas.mem_total >= 4096);
    stats_t global;
    uint32_t nodos = 0;
    assert(sistema_stats_global(&global, &nodos) == OK);
    assert(nodos == 2);
    assert(global.mem_usada == 8192);
    sistema_print_global_page_table();

    enviar(al_hijo[1], 'Q');
    esperar(al_padre[0], 'D');
    enviar(al_hijo[1], 'X');
    assert(sistema_finalize() == OK);
    int estado;
    assert(waitpid(pid, &estado, 0) == pid);
    assert(WIFEXITED(estado) && WEXITSTATUS(estado) == 0);
    remove(ruta);
    puts("Consultas globales: tablas remotas, memoria actual y resumen de 2 nodos correctos");
    return 0;
}
