#define _POSIX_C_SOURCE 200809L
#include "../sistema/api/api.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAG 4096u
#define MIB (1024u * 1024u)
#define PRUEBA_BYTES (2u * MIB + 512u * 1024u)

static void conf(char *ruta, size_t cap, pid_t padre, unsigned nodo)
{
    snprintf(ruta, cap, "/tmp/dsm_multinodo_%ld_%u.conf", (long)padre, nodo);
    FILE *f = fopen(ruta, "w");
    assert(f != NULL);
    fprintf(f, "nodoId=%u\n", nodo);
    fprintf(f, "ipLocal=127.0.0.1\n");
    fprintf(f, "puertoBase=%u\n", 28040 + nodo * 10);
    fprintf(f, "puertoNodos=%u\n", 28041 + nodo * 10);
    fprintf(f, "puertoHeartbeat=%u\n", 28042 + nodo * 10);
    fprintf(f, "puertoPages=%u\n", 28043 + nodo * 10);
    fprintf(f, "puertoConsulta=%u\n", 28044 + nodo * 10);
    fprintf(f, "logArchivo=/tmp/dsm_multinodo_%ld_%u.log\n", (long)padre, nodo);
    assert(fclose(f) == 0);
}

static void enviar(int fd, char dato) { assert(write(fd, &dato, 1) == 1); }
static void recibir(int fd, char dato)
{
    char respuesta = 0;
    assert(read(fd, &respuesta, 1) == 1 && respuesta == dato);
}

static void hijo(unsigned nodo, pid_t padre, int desde_padre, int hacia_padre)
{
    char ruta[128];
    conf(ruta, sizeof(ruta), padre, nodo);
    assert(sistema_init(ruta) == OK);
    assert(sistema_red_iniciar() == OK);
    enviar(hacia_padre, 'R');
    if (nodo == 2) {
        /* Ocupar capacidad del segundo respaldo para provocar rollback. */
        recibir(desde_padre, 'B');
        void *ocupacion = sistema_alloc(512u * 1024u);
        assert(ocupacion != NULL);
        enviar(hacia_padre, 'C');
        recibir(desde_padre, 'D');
        assert(sistema_free(ocupacion) == OK);
        enviar(hacia_padre, 'E');
    }
    recibir(desde_padre, 'A');
    stats_t st;
    assert(sistema_stats((int)nodo, &st) == OK);
    assert(st.mem_usada == (nodo == 1 ? MIB : 512u * 1024u));
    enviar(hacia_padre, 'M');
    recibir(desde_padre, 'L');
    assert(sistema_stats((int)nodo, &st) == OK);
    assert(st.mem_usada == 0);
    enviar(hacia_padre, 'F');
    assert(sistema_finalize() == OK);
    remove(ruta);
    _exit(0);
}

int main(void)
{
    alarm(50);
    pid_t padre = getpid();
    int respuesta[2][2], orden[2][2];
    pid_t hijos[2];
    for (unsigned i = 0; i < 2; ++i) {
        assert(pipe(respuesta[i]) == 0);
        assert(pipe(orden[i]) == 0);
        hijos[i] = fork();
        assert(hijos[i] >= 0);
        if (hijos[i] == 0) {
            close(respuesta[i][0]);
            close(orden[i][1]);
            for (unsigned j = 0; j < i; ++j) {
                close(respuesta[j][0]);
                close(orden[j][1]);
            }
            hijo(i + 1, padre, orden[i][0], respuesta[i][1]);
        }
        close(respuesta[i][1]);
        close(orden[i][0]);
    }
    recibir(respuesta[0][0], 'R');
    recibir(respuesta[1][0], 'R');
    char ruta[128];
    conf(ruta, sizeof(ruta), padre, 0);
    assert(sistema_init(ruta) == OK);
    assert(sistema_red_iniciar() == OK);
    assert(sistema_red_unirse("127.0.0.1", 28051) == OK);
    assert(sistema_red_unirse("127.0.0.1", 28061) == OK);

    enviar(orden[1][1], 'B');
    recibir(respuesta[1][0], 'C');
    /* El primero aporta 1 MiB y el segundo solo 512 KiB: la solicitud
       mayor debe fallar sin dejar reservas remotas pendientes. */
    assert(sistema_alloc(2u * MIB + 768u * 1024u) == NULL);
    stats_t antes;
    assert(sistema_stats(0, &antes) == OK && antes.mem_usada == 0);
    enviar(orden[1][1], 'D');
    recibir(respuesta[1][0], 'E');

    /* 1 MiB local + 1 MiB en nodo 1 + 512 KiB en nodo 2. */
    unsigned char *region = sistema_alloc(PRUEBA_BYTES);
    assert(region != NULL);
    for (unsigned i = 0; i < 2; ++i) enviar(orden[i][1], 'A');
    for (unsigned i = 0; i < 2; ++i) recibir(respuesta[i][0], 'M');
    region[0] = 21;
    region[MIB - 1] = 22;
    assert(region[MIB] == 0);
    assert(region[2u * MIB - PAG] == 0);
    assert(region[2u * MIB] == 0);
    assert(region[PRUEBA_BYTES - PAG] == 0);
    region[MIB] = 31;
    region[2u * MIB - PAG] = 32;
    region[2u * MIB] = 41;
    region[PRUEBA_BYTES - PAG] = 42;
    assert(region[MIB] == 31 && region[2u * MIB - PAG] == 32);
    assert(region[2u * MIB] == 41 && region[PRUEBA_BYTES - PAG] == 42);
    stats_t st;
    assert(sistema_stats(0, &st) == OK);
    assert(st.paginas_recibidas >= 8);
    assert(st.mem_usada == MIB);
    assert(sistema_free(region) == OK);
    for (unsigned i = 0; i < 2; ++i) enviar(orden[i][1], 'L');
    for (unsigned i = 0; i < 2; ++i) recibir(respuesta[i][0], 'F');
    assert(sistema_finalize() == OK);
    for (unsigned i = 0; i < 2; ++i) {
        int estado;
        assert(waitpid(hijos[i], &estado, 0) == hijos[i]);
        assert(WIFEXITED(estado) && WEXITSTATUS(estado) == 0);
        close(respuesta[i][0]);
        close(orden[i][1]);
    }
    remove(ruta);
    puts("Reserva multinodo: 1 MiB local + 1 MiB nodo 1 + 512 KiB nodo 2 correctos");
    return 0;
}
