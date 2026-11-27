#define _POSIX_C_SOURCE 200809L
#include "../sistema/api/api.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define HILOS 8

typedef struct {
    pthread_barrier_t *inicio;
    volatile char *pagina;
    int correcto;
} lector_t;

static void *leer_en_paralelo(void *arg)
{
    lector_t *lector = arg;
    int r = pthread_barrier_wait(lector->inicio);
    assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
    lector->correcto = lector->pagina[0] == 'F';
    return NULL;
}

static void avisar(int fd, char mensaje)
{
    assert(write(fd, &mensaje, 1) == 1);
}

static void esperar(int fd, char mensaje)
{
    char recibido = 0;
    assert(read(fd, &recibido, 1) == 1);
    assert(recibido == mensaje);
}

static void archivo_config(char *ruta, size_t tamano, pid_t padre, unsigned id)
{
    int n = snprintf(ruta, tamano, "/tmp/dsm_msi_hilos_%ld_%u.conf",
                     (long)padre, id);
    assert(n > 0 && (size_t)n < tamano);
    FILE *f = fopen(ruta, "w");
    assert(f != NULL);
    fprintf(f, "nodoId=%u\nipLocal=127.0.0.1\n", id);
    fprintf(f, "puertoBase=%u\npuertoNodos=%u\n", 28060 + 10 * id,
            28061 + 10 * id);
    fprintf(f, "puertoHeartbeat=%u\npuertoPages=%u\npuertoConsulta=%u\n",
            28062 + 10 * id, 28063 + 10 * id, 28064 + 10 * id);
    fprintf(f, "logArchivo=/tmp/dsm_msi_hilos_%ld_%u.log\n", (long)padre, id);
    assert(fclose(f) == 0);
}

static void comprobar_hilos(volatile char *pagina)
{
    pthread_t hilos[HILOS];
    lector_t lectores[HILOS];
    pthread_barrier_t inicio;
    assert(pthread_barrier_init(&inicio, NULL, HILOS + 1) == 0);
    for (int i = 0; i < HILOS; ++i) {
        lectores[i] = (lector_t){.inicio = &inicio, .pagina = pagina, .correcto = 0};
        assert(pthread_create(&hilos[i], NULL, leer_en_paralelo,
                              &lectores[i]) == 0);
    }
    int r = pthread_barrier_wait(&inicio);
    assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
    for (int i = 0; i < HILOS; ++i) {
        assert(pthread_join(hilos[i], NULL) == 0);
        assert(lectores[i].correcto);
    }
    assert(pthread_barrier_destroy(&inicio) == 0);
}

static void ejecutar_hijo(unsigned id, pid_t padre, int entrada, int salida)
{
    char ruta[128];
    archivo_config(ruta, sizeof(ruta), padre, id);
    assert(sistema_init(ruta) == OK);
    char *p = sistema_alloc(4096);
    assert(p != NULL);
    assert(sistema_page_set_state(0, PAGE_INVALID) == OK);
    assert(sistema_page_registrar_origen(0, 0) == OK);
    assert(sistema_red_iniciar() == OK);
    avisar(salida, 'R');
    esperar(entrada, 'J');
    assert(sistema_red_unirse("127.0.0.1", 28061) == OK);
    avisar(salida, 'C');
    esperar(entrada, 'L');
    assert(strcmp(p, "Inicial nodo 0") == 0);
    avisar(salida, 'A');

    if (id == 1) {
        esperar(entrada, 'W');
        strcpy(p, "Actualizado nodo 1");
        page_state_t msi;
        assert(sistema_page_get_state(0, &msi) == OK && msi == PAGE_MODIFIED);
        avisar(salida, 'M');
        esperar(entrada, 'V');
        assert(sistema_page_get_state(0, &msi) == OK && msi == PAGE_INVALID);
        avisar(salida, 'I');
    } else {
        esperar(entrada, 'V');
        page_state_t msi;
        assert(sistema_page_get_state(0, &msi) == OK && msi == PAGE_INVALID);
        avisar(salida, 'I');
        esperar(entrada, 'L');
        assert(strcmp(p, "Actualizado nodo 1") == 0);
        avisar(salida, 'A');
        esperar(entrada, 'T');
        assert(sistema_page_get_state(0, &msi) == OK && msi == PAGE_INVALID);
        /* Ocho hilos leen simultaneamente una pagina INVALID.
           Un unico fetch debe bastar para resolver los accesos. */
        comprobar_hilos((volatile char *)p);
        assert(sistema_page_get_state(0, &msi) == OK && msi == PAGE_SHARED);
        stats_t stats;
        assert(sistema_stats((int)id, &stats) == OK);
        assert(stats.paginas_recibidas == 3);
        avisar(salida, 'T');
    }
    esperar(entrada, 'F');
    assert(sistema_free(p) == OK);
    assert(sistema_finalize() == OK);
    remove(ruta);
    _exit(0);
}

int main(void)
{
    alarm(35);
    pid_t padre = getpid();
    int al_hijo[2][2], del_hijo[2][2];
    pid_t hijos[2];
    for (int i = 0; i < 2; ++i) {
        assert(pipe(al_hijo[i]) == 0);
        assert(pipe(del_hijo[i]) == 0);
    }
    for (int i = 0; i < 2; ++i) {
        hijos[i] = fork();
        assert(hijos[i] >= 0);
        if (hijos[i] == 0) {
            for (int j = 0; j < 2; ++j) {
                close(al_hijo[j][1]);
                close(del_hijo[j][0]);
                if (j != i) {
                    close(al_hijo[j][0]);
                    close(del_hijo[j][1]);
                }
            }
            ejecutar_hijo((unsigned)i + 1, padre, al_hijo[i][0], del_hijo[i][1]);
        }
    }
    for (int i = 0; i < 2; ++i) {
        close(al_hijo[i][0]);
        close(del_hijo[i][1]);
    }

    char ruta[128];
    archivo_config(ruta, sizeof(ruta), padre, 0);
    assert(sistema_init(ruta) == OK);
    char *p = sistema_alloc(4096);
    assert(p != NULL);
    strcpy(p, "Inicial nodo 0");
    assert(sistema_red_iniciar() == OK);
    for (int i = 0; i < 2; ++i) esperar(del_hijo[i][0], 'R');
    for (int i = 0; i < 2; ++i) avisar(al_hijo[i][1], 'J');
    for (int i = 0; i < 2; ++i) esperar(del_hijo[i][0], 'C');
    assert(sistema_node_cantidad() == 3);
    for (int i = 0; i < 2; ++i) avisar(al_hijo[i][1], 'L');
    for (int i = 0; i < 2; ++i) esperar(del_hijo[i][0], 'A');

    /* Nodo 1 solicita exclusividad cuando nodo 2 tambien tiene copia. */
    avisar(al_hijo[0][1], 'W');
    esperar(del_hijo[0][0], 'M');
    page_state_t msi;
    assert(sistema_page_get_state(0, &msi) == OK && msi == PAGE_INVALID);
    avisar(al_hijo[1][1], 'V');
    esperar(del_hijo[1][0], 'I');

    /* El origen recupera el valor actualizado desde el escritor remoto. */
    assert(strcmp(p, "Actualizado nodo 1") == 0);
    avisar(al_hijo[1][1], 'L');
    esperar(del_hijo[1][0], 'A');

    /* El origen recupera escritura e invalida ambas copias compartidas. */
    strcpy(p, "Final nodo 0");
    avisar(al_hijo[0][1], 'V');
    esperar(del_hijo[0][0], 'I');
    avisar(al_hijo[1][1], 'T');
    esperar(del_hijo[1][0], 'T');

    for (int i = 0; i < 2; ++i) avisar(al_hijo[i][1], 'F');
    for (int i = 0; i < 2; ++i) {
        int estado = 0;
        assert(waitpid(hijos[i], &estado, 0) == hijos[i]);
        assert(WIFEXITED(estado) && WEXITSTATUS(estado) == 0);
        close(al_hijo[i][1]);
        close(del_hijo[i][0]);
    }
    assert(sistema_free(p) == OK);
    assert(sistema_finalize() == OK);
    remove(ruta);
    puts("MSI: 3 nodos, invalidacion de copias y lectores concurrentes correctos");
    return 0;
}
