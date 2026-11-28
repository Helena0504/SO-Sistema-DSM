#define _POSIX_C_SOURCE 200809L
#include "../sistema/api/api.h"
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int to_child[2], to_parent[2];
static atomic_int local_adquirido;

static void escribir(int fd, char c) { assert(write(fd, &c, 1) == 1); }
static void leer(int fd, char esperado)
{
    char c = 0;
    assert(read(fd, &c, 1) == 1 && c == esperado);
}
static int hay_mensaje(int fd)
{
    fd_set fds;
    FD_ZERO(&fds); FD_SET(fd, &fds);
    struct timeval t = {.tv_sec = 0, .tv_usec = 150000};
    return select(fd + 1, &fds, NULL, NULL, &t) == 1;
}
static void configurar(char *ruta, size_t n, pid_t padre, int nodo)
{
    snprintf(ruta, n, "/tmp/dsm_sync_%ld_%d.conf", (long)padre, nodo);
    FILE *f = fopen(ruta, "w");
    assert(f != NULL);
    fprintf(f, "nodoId=%d\nipLocal=127.0.0.1\n", nodo);
    fprintf(f, "puertoBase=%d\npuertoNodos=%d\npuertoHeartbeat=%d\npuertoPages=%d\npuertoConsulta=%d\n",
            28075 + nodo * 10, 28076 + nodo * 10, 28077 + nodo * 10,
            28078 + nodo * 10, 28079 + nodo * 10);
    fprintf(f, "logArchivo=/tmp/dsm_sync_%ld_%d.log\n", (long)padre, nodo);
    assert(fclose(f) == 0);
}
static void *bloqueo_en_hilo(void *arg)
{
    (void)arg;
    assert(sistema_lock(7) == OK);
    atomic_store(&local_adquirido, 1);
    assert(sistema_unlock(7) == OK);
    return NULL;
}
static void hijo(pid_t padre)
{
    close(to_child[1]); close(to_parent[0]);
    char ruta[120]; configurar(ruta, sizeof(ruta), padre, 1);
    assert(sistema_init(ruta) == OK);
    assert(sistema_red_iniciar() == OK);
    escribir(to_parent[1], 'R');
    leer(to_child[0], 'I');
    assert(sistema_red_unirse("127.0.0.1", 28076) == OK);
    assert(sistema_wait_ready() == OK);
    escribir(to_parent[1], 'J');
    leer(to_child[0], 'T');
    assert(sistema_lock(7) == OK);
    escribir(to_parent[1], 'L');
    leer(to_child[0], 'U');
    assert(sistema_unlock(7) == OK);
    escribir(to_parent[1], 'O');
    for (int i = 0; i < 2; ++i) {
        leer(to_child[0], 'B');
        escribir(to_parent[1], 'W');
        assert(sistema_block_all() == OK);
        escribir(to_parent[1], 'P');
    }
    leer(to_child[0], 'X');
    assert(sistema_finalize() == OK);
    remove(ruta);
    _exit(0);
}
int main(void)
{
    alarm(25);
    assert(pipe(to_child) == 0 && pipe(to_parent) == 0);
    pid_t padre = getpid();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) hijo(padre);
    close(to_child[0]); close(to_parent[1]);
    leer(to_parent[0], 'R');
    char ruta[120]; configurar(ruta, sizeof(ruta), padre, 0);
    assert(sistema_init(ruta) == OK);
    assert(sistema_red_iniciar() == OK);
    escribir(to_child[1], 'I');
    leer(to_parent[0], 'J');
    assert(sistema_node_cantidad() == 2);
    escribir(to_child[1], 'T');
    leer(to_parent[0], 'L');
    pthread_t hilo;
    atomic_store(&local_adquirido, 0);
    assert(pthread_create(&hilo, NULL, bloqueo_en_hilo, NULL) == 0);
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 150000000};
    nanosleep(&ts, NULL);
    assert(atomic_load(&local_adquirido) == 0);
    escribir(to_child[1], 'U');
    leer(to_parent[0], 'O');
    assert(pthread_join(hilo, NULL) == 0);
    assert(atomic_load(&local_adquirido) == 1);
    for (int i = 0; i < 2; ++i) {
        escribir(to_child[1], 'B');
        leer(to_parent[0], 'W');
        assert(!hay_mensaje(to_parent[0]));
        assert(sistema_block_all() == OK);
        leer(to_parent[0], 'P');
    }
    escribir(to_child[1], 'X');
    assert(sistema_finalize() == OK);
    int estado = 0;
    assert(waitpid(pid, &estado, 0) == pid);
    assert(WIFEXITED(estado) && WEXITSTATUS(estado) == 0);
    remove(ruta);
    puts("Sincronizacion: semaforos POSIX, lock distribuido y barrera de 2 nodos correctos");
    return 0;
}
