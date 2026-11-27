#define _POSIX_C_SOURCE 200809L
#include "../sistema/api/api.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGINA 4096u

static void crear_config(char *ruta, size_t capacidad, pid_t padre, unsigned id)
{
    snprintf(ruta, capacidad, "/tmp/dsm_regiones_%ld_%u.conf", (long)padre, id);
    FILE *f = fopen(ruta, "w");
    assert(f != NULL);
    fprintf(f, "nodoId=%u\n", id);
    fprintf(f, "ipLocal=127.0.0.1\n");
    fprintf(f, "puertoBase=%u\n", 28040 + 10 * id);
    fprintf(f, "puertoNodos=%u\n", 28041 + 10 * id);
    fprintf(f, "puertoHeartbeat=%u\n", 28042 + 10 * id);
    fprintf(f, "puertoPages=%u\n", 28043 + 10 * id);
    fprintf(f, "puertoConsulta=%u\n", 28044 + 10 * id);
    fprintf(f, "logArchivo=/tmp/dsm_regiones_%ld_%u.log\n", (long)padre, id);
    assert(fclose(f) == 0);
}

static void enviar(int fd, char caracter)
{
    assert(write(fd, &caracter, 1) == 1);
}

static void recibir(int fd, char esperado)
{
    char caracter = '\0';
    assert(read(fd, &caracter, 1) == 1 && caracter == esperado);
}

int main(void)
{
    alarm(30);
    pid_t padre = getpid();
    int hacia_hijo[2], hacia_padre[2];
    assert(pipe(hacia_hijo) == 0 && pipe(hacia_padre) == 0);
    pid_t hijo = fork();
    assert(hijo >= 0);

    if (hijo == 0) {
        close(hacia_hijo[1]);
        close(hacia_padre[0]);
        char ruta[128];
        crear_config(ruta, sizeof(ruta), padre, 1);
        assert(sistema_init(ruta) == OK);
        char *primera = sistema_alloc(2 * PAGINA);
        char *segunda = sistema_alloc(3 * PAGINA);
        char *solo_local = sistema_alloc(PAGINA);
        assert(primera != NULL && segunda != NULL && solo_local != NULL);
        strcpy(solo_local, "Esta pagina permanece local");

        /* Una unica llamada registra TODAS las paginas de cada reserva. */
        assert(sistema_region_remota(primera + 1, 0) != OK);
        assert(sistema_region_remota(primera, 1) != OK);
        assert(sistema_region_remota(primera, 0) == OK);
        assert(sistema_region_remota(segunda, 0) == OK);
        assert(sistema_region_remota(primera, 0) != OK);
        for (uint32_t id = 0; id < 5; ++id) {
            page_state_t estado;
            assert(sistema_page_get_state(id, &estado) == OK);
            assert(estado == PAGE_INVALID);
        }
        assert(sistema_red_iniciar() == OK);
        recibir(hacia_hijo[0], 'R');
        assert(sistema_red_unirse("127.0.0.1", 28041) == OK);

        /* Orden desordenado: accede a paginas de distintas regiones. */
        assert(strcmp(segunda + 2 * PAGINA, "pagina 4") == 0);
        assert(strcmp(primera + PAGINA, "pagina 1") == 0);
        assert(strcmp(segunda, "pagina 2") == 0);
        assert(strcmp(primera, "pagina 0") == 0);
        assert(strcmp(segunda + PAGINA, "pagina 3") == 0);
        assert(strcmp(solo_local, "Esta pagina permanece local") == 0);
        stats_t estadisticas;
        assert(sistema_stats(1, &estadisticas) == OK);
        assert(estadisticas.paginas_recibidas == 5);
        assert(strcmp(segunda, "pagina 2") == 0);
        assert(sistema_stats(1, &estadisticas) == OK);
        assert(estadisticas.paginas_recibidas == 5);
        enviar(hacia_padre[1], 'P');

        recibir(hacia_hijo[0], 'I');
        page_state_t estado;
        assert(sistema_page_get_state(4, &estado) == OK);
        assert(estado == PAGE_INVALID);
        assert(strcmp(segunda + 2 * PAGINA, "pagina 4 actualizada") == 0);
        assert(sistema_stats(1, &estadisticas) == OK);
        assert(estadisticas.paginas_recibidas == 6);
        assert(sistema_free(solo_local) == OK);
        assert(sistema_free(segunda) == OK);
        assert(sistema_free(primera) == OK);
        enviar(hacia_padre[1], 'S');
        assert(sistema_finalize() == OK);
        remove(ruta);
        close(hacia_hijo[0]); close(hacia_padre[1]);
        _exit(0);
    }

    close(hacia_hijo[0]);
    close(hacia_padre[1]);
    char ruta[128];
    crear_config(ruta, sizeof(ruta), padre, 0);
    assert(sistema_init(ruta) == OK);
    char *primera = sistema_alloc(2 * PAGINA);
    char *segunda = sistema_alloc(3 * PAGINA);
    assert(primera != NULL && segunda != NULL);
    strcpy(primera, "pagina 0");
    strcpy(primera + PAGINA, "pagina 1");
    strcpy(segunda, "pagina 2");
    strcpy(segunda + PAGINA, "pagina 3");
    strcpy(segunda + 2 * PAGINA, "pagina 4");
    assert(sistema_red_iniciar() == OK);
    enviar(hacia_hijo[1], 'R');
    recibir(hacia_padre[0], 'P');
    /* La escritura en la quinta pagina invalida solo su copia remota. */
    strcpy(segunda + 2 * PAGINA, "pagina 4 actualizada");
    enviar(hacia_hijo[1], 'I');
    recibir(hacia_padre[0], 'S');
    int resultado;
    assert(waitpid(hijo, &resultado, 0) == hijo);
    assert(WIFEXITED(resultado) && WEXITSTATUS(resultado) == 0);
    assert(sistema_free(segunda) == OK);
    assert(sistema_free(primera) == OK);
    assert(sistema_finalize() == OK);
    remove(ruta);
    close(hacia_hijo[1]); close(hacia_padre[0]);
    puts("Regiones DSM: 5 paginas remotas y recuperacion automatica correcta");
    return 0;
}
