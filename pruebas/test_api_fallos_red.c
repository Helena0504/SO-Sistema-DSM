#define _POSIX_C_SOURCE 200809L
#include "../sistema/api/api.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

static void generar_config(char *ruta, size_t capacidad, unsigned id)
{
    snprintf(ruta, capacidad, "/tmp/dsm_api_auto_%ld_%u.conf", (long)getppid(), id);
    FILE *f = fopen(ruta, "w");
    assert(f != NULL);
    fprintf(f, "nodoId=%u\n", id);
    fprintf(f, "ipLocal=127.0.0.1\n");
    fprintf(f, "puertoBase=%u\n", 28060 + 10 * id);
    fprintf(f, "puertoNodos=%u\n", 28061 + 10 * id);
    fprintf(f, "puertoHeartbeat=%u\n", 28062 + 10 * id);
    fprintf(f, "puertoPages=%u\n", 28063 + 10 * id);
    fprintf(f, "puertoConsulta=%u\n", 28064 + 10 * id);
    fprintf(f, "logArchivo=/tmp/dsm_api_auto_%ld_%u.log\n", (long)getppid(), id);
    assert(fclose(f) == 0);
}

static void informar(int fd, char c)
{
    assert(write(fd, &c, 1) == 1);
}

static void escuchar(int fd, char c)
{
    char recibido;
    assert(read(fd, &recibido, 1) == 1 && recibido == c);
}

int main(void)
{
    alarm(25);
    int a[2], b[2];
    assert(pipe(a) == 0 && pipe(b) == 0);
    pid_t hijo = fork();
    assert(hijo >= 0);
    if (hijo == 0) {
        close(a[1]); close(b[0]);
        char ruta[128];
        generar_config(ruta, sizeof(ruta), 1);
        assert(sistema_init(ruta) == OK);
        char *p = sistema_alloc(4096);
        assert(p != NULL);
        assert(sistema_page_set_state(0, PAGE_INVALID) == OK);
        assert(sistema_page_registrar_origen(0, 0) == OK);
        assert(sistema_red_iniciar() == OK);
        escuchar(a[0], 'R');
        assert(sistema_red_unirse("127.0.0.1", 28061) == OK);
        assert(sistema_node_cantidad() == 2);
        assert(sistema_red_heartbeat(0) == OK);
        /* Sin pedir pagina: el primer acceso dispara SIGSEGV y PAGE_REQUEST. */
        assert(strcmp(p, "Página enviada desde API") == 0);
        page_state_t msi;
        assert(sistema_page_get_state(0, &msi) == OK && msi == PAGE_SHARED);
        informar(b[1], 'P');
        escuchar(a[0], 'I');
        assert(sistema_page_get_state(0, &msi) == OK && msi == PAGE_INVALID);
        /* Despues de la invalidacion, otra lectura vuelve a pedir la pagina. */
        assert(strcmp(p, "Origen recupero escritura") == 0);
        informar(b[1], 'S');
        assert(sistema_finalize() == OK);
        remove(ruta);
        close(a[0]); close(b[1]);
        _exit(0);
    }
    close(a[0]); close(b[1]);
    char ruta[128];
    generar_config(ruta, sizeof(ruta), 0);
    assert(sistema_init(ruta) == OK);
    char *p = sistema_alloc(4096);
    assert(p != NULL);
    strcpy(p, "Página enviada desde API");
    assert(sistema_red_iniciar() == OK);
    informar(a[1], 'R');
    escuchar(b[0], 'P');
    assert(sistema_node_cantidad() == 2);
    /* La escritura en SHARED invalida automaticamente la copia remota. */
    strcpy(p, "Origen recupero escritura");
    informar(a[1], 'I');
    escuchar(b[0], 'S');
    int estado;
    assert(waitpid(hijo, &estado, 0) == hijo);
    assert(WIFEXITED(estado) && WEXITSTATUS(estado) == 0);
    assert(sistema_finalize() == OK);
    remove(ruta);
    close(a[1]); close(b[0]);
    puts("Paginacion bajo demanda y MSI entre procesos: correcta");
    return 0;
}
