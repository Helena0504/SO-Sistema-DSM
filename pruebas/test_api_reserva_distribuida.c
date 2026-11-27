#define _POSIX_C_SOURCE 200809L
#include "../sistema/api/api.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAG 4096u
#define LOCAL_BYTES (1024u * 1024u)
#define EXTRA_BYTES (512u * 1024u)

static void configuracion(char *ruta, size_t capacidad, pid_t padre, unsigned id)
{
    snprintf(ruta, capacidad, "/tmp/dsm_distribuida_%ld_%u.conf", (long)padre, id);
    FILE *f = fopen(ruta, "w");
    assert(f != NULL);
    fprintf(f, "nodoId=%u\n", id);
    fprintf(f, "ipLocal=127.0.0.1\n");
    fprintf(f, "puertoBase=%u\n", 28060 + id * 10);
    fprintf(f, "puertoNodos=%u\n", 28061 + id * 10);
    fprintf(f, "puertoHeartbeat=%u\n", 28062 + id * 10);
    fprintf(f, "puertoPages=%u\n", 28063 + id * 10);
    fprintf(f, "puertoConsulta=%u\n", 28064 + id * 10);
    fprintf(f, "logArchivo=/tmp/dsm_distribuida_%ld_%u.log\n", (long)padre, id);
    assert(fclose(f) == 0);
}

static void enviar(int fd, char c)
{
    assert(write(fd, &c, 1) == 1);
}

static void recibir(int fd, char c)
{
    char recibido = 0;
    assert(read(fd, &recibido, 1) == 1 && recibido == c);
}

int main(void)
{
    alarm(35);
    pid_t padre = getpid();
    int hijo_a_padre[2], padre_a_hijo[2];
    assert(pipe(hijo_a_padre) == 0 && pipe(padre_a_hijo) == 0);
    pid_t hijo = fork();
    assert(hijo >= 0);

    if (hijo == 0) {
        close(hijo_a_padre[0]);
        close(padre_a_hijo[1]);
        char ruta[128];
        configuracion(ruta, sizeof(ruta), padre, 1);
        assert(sistema_init(ruta) == OK);
        assert(sistema_red_iniciar() == OK);
        enviar(hijo_a_padre[1], 'R');
        recibir(padre_a_hijo[0], 'A');
        stats_t st;
        assert(sistema_stats(1, &st) == OK);
        assert(st.mem_usada == EXTRA_BYTES + 256u * 1024u);
        enviar(hijo_a_padre[1], 'B');
        recibir(padre_a_hijo[0], 'W');
        /* El nodo de respaldo debe recuperar los datos del escritor actual. */
        assert(sistema_red_pedir_pagina(0, 256) == OK);
        page_state_t msi;
        assert(sistema_page_get_state(256, &msi) == OK && msi == PAGE_SHARED);
        enviar(hijo_a_padre[1], 'D');
        recibir(padre_a_hijo[0], 'X');
        assert(sistema_page_get_state(256, &msi) == OK && msi == PAGE_INVALID);
        enviar(hijo_a_padre[1], 'Y');
        recibir(padre_a_hijo[0], 'L');
        assert(sistema_stats(1, &st) == OK);
        assert(st.mem_usada == 0);
        enviar(hijo_a_padre[1], 'F');
        assert(sistema_finalize() == OK);
        remove(ruta);
        _exit(0);
    }

    close(hijo_a_padre[1]);
    close(padre_a_hijo[0]);
    recibir(hijo_a_padre[0], 'R');
    char ruta[128];
    configuracion(ruta, sizeof(ruta), padre, 0);
    assert(sistema_init(ruta) == OK);
    assert(sistema_red_iniciar() == OK);
    assert(sistema_red_unirse("127.0.0.1", 28071) == OK);
    assert(sistema_alloc(3u * LOCAL_BYTES) == NULL);
    /* El sistema debe reservar 1 MiB local y 512 KiB en el nodo 1. */
    unsigned char *memoria = sistema_alloc(LOCAL_BYTES + EXTRA_BYTES);
    assert(memoria != NULL);
    /* Una segunda reserva ocupa el espacio remoto que queda disponible. */
    unsigned char *otra = sistema_alloc(256u * 1024u);
    assert(otra != NULL);
    memoria[0] = 42;
    memoria[LOCAL_BYTES - 1] = 99;
    assert(memoria[0] == 42 && memoria[LOCAL_BYTES - 1] == 99);
    enviar(padre_a_hijo[1], 'A');
    recibir(hijo_a_padre[0], 'B');

    /* No hay llamadas a pedir_pagina: cada lectura dispara SIGSEGV y TCP. */
    assert(memoria[LOCAL_BYTES] == 0);
    assert(memoria[LOCAL_BYTES + EXTRA_BYTES - PAG] == 0);
    /* El acceso de escritura eleva una pagina remota SHARED a MODIFIED. */
    memoria[LOCAL_BYTES] = 123;
    memoria[LOCAL_BYTES + EXTRA_BYTES - PAG] = 77;
    assert(otra[0] == 0);
    otra[0] = 33;
    assert(otra[0] == 33);
    assert(memoria[LOCAL_BYTES] == 123);
    assert(memoria[LOCAL_BYTES + EXTRA_BYTES - PAG] == 77);
    stats_t st;
    assert(sistema_stats(0, &st) == OK);
    assert(st.paginas_recibidas == 6);
    assert(st.mem_usada == LOCAL_BYTES);
    enviar(padre_a_hijo[1], 'W');
    recibir(hijo_a_padre[0], 'D');
    /* Tras ceder una lectura, una nueva escritura vuelve a invalidar el respaldo. */
    memoria[LOCAL_BYTES] = 56;
    assert(memoria[LOCAL_BYTES] == 56);
    enviar(padre_a_hijo[1], 'X');
    recibir(hijo_a_padre[0], 'Y');
    assert(sistema_free(otra) == OK);
    assert(sistema_free(memoria) == OK);
    enviar(padre_a_hijo[1], 'L');
    recibir(hijo_a_padre[0], 'F');
    assert(sistema_finalize() == OK);
    int estado = 0;
    assert(waitpid(hijo, &estado, 0) == hijo);
    assert(WIFEXITED(estado) && WEXITSTATUS(estado) == 0);
    remove(ruta);
    puts("Reserva distribuida: 1 MiB local + 768 KiB remoto, lectura y escritura correctas");
    return 0;
}
