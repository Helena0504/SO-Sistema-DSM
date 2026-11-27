#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include "../../api/api.h"
#include "../../global/config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define HTTP_PUERTO 28005
#define HTTP_BUFFER 8192

static int servidor = -1;
static int activo = 0;
static pthread_t hilo;
static const config_t *config_actual = NULL;

static int enviar_todo(int fd, const char *texto)
{
    size_t restante = strlen(texto);
    while (restante > 0) {
        ssize_t enviados = send(fd, texto, restante, 0);
        if (enviados <= 0) return -1;
        texto += enviados;
        restante -= (size_t)enviados;
    }
    return 0;
}

static void responder(int cliente)
{
    char solicitud[1024];
    (void)recv(cliente, solicitud, sizeof(solicitud) - 1, 0);

    char html[HTTP_BUFFER];
    size_t usados = 0;

    usados += (size_t)snprintf(html + usados, sizeof(html) - usados,
        "<!DOCTYPE html><html lang=\"es\"><head>"
        "<meta charset=\"UTF-8\"><meta http-equiv=\"refresh\" content=\"2\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>DSM - Monitor</title>"
        "<style>body{font-family:Arial;margin:30px;background:#f4f4f4}"
        "table{border-collapse:collapse;width:100%%;background:white}"
        "th,td{padding:10px;border:1px solid #ccc;text-align:left}"
        "th{background:#263746;color:white}.ok{color:green}"
        ".down{color:red}</style></head><body>"
        "<h1>Sistema DSM - Monitor de nodos</h1>"
        "<p>Nodo local: %u | IP: %s</p>"
        "<p>Nodos conectados: %d/%u</p>"
        "<table><tr><th>ID</th><th>IP</th><th>Estado</th>"
        "<th>Memoria usada</th><th>Memoria total</th>"
        "<th>Enviadas</th><th>Recibidas</th></tr>",
        config_actual->nodo_id, config_actual->ip_local,
        sistema_node_cantidad(), config_actual->cantidad_nodos);

    for (uint32_t i = 0; i < config_actual->cantidad_nodos; i++) {
        const config_nodo_t *nodo = &config_actual->nodos[i];
        node_info_t info;
        stats_t stats;
        int conectado = sistema_node_info((int)nodo->id, &info) == OK &&
                        info.estado;
        int tiene_stats = conectado &&
                          sistema_stats((int)nodo->id, &stats) == OK;

        usados += (size_t)snprintf(html + usados, sizeof(html) - usados,
            "<tr><td>%u</td><td>%s</td><td class=\"%s\">%s</td>"
            "<td>%llu</td><td>%llu</td><td>%llu</td><td>%llu</td></tr>",
            nodo->id, nodo->ip, conectado ? "ok" : "down",
            conectado ? "CONECTADO" : "DESCONECTADO",
            (unsigned long long)(tiene_stats ? stats.mem_usada : 0),
            (unsigned long long)(tiene_stats ? stats.mem_total : 0),
            (unsigned long long)(tiene_stats ? stats.paginas_enviadas : 0),
            (unsigned long long)(tiene_stats ? stats.paginas_recibidas : 0));

        if (usados >= sizeof(html) - 512) break;
    }

    usados += (size_t)snprintf(html + usados, sizeof(html) - usados,
        "</table><p>Actualizacion automatica cada 2 segundos.</p>"
        "</body></html>");

    char cabecera[256];
    snprintf(cabecera, sizeof(cabecera),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n", usados);

    (void)enviar_todo(cliente, cabecera);
    (void)enviar_todo(cliente, html);
}

static void *hilo_http(void *arg)
{
    (void)arg;
    while (activo) {
        struct pollfd evento = {.fd = servidor, .events = POLLIN};
        int resultado = poll(&evento, 1, 500);
        if (resultado <= 0) continue;
        if (!(evento.revents & POLLIN)) continue;

        int cliente = accept(servidor, NULL, NULL);
        if (cliente >= 0) {
            responder(cliente);
            close(cliente);
        }
    }
    return NULL;
}

int http_iniciar(const config_t *config)
{
    if (config == NULL) return -1;
    config_actual = config;
    struct sockaddr_in direccion = {0};

    servidor = socket(AF_INET, SOCK_STREAM, 0);
    if (servidor < 0) return -1;

    int reutilizar = 1;
    setsockopt(servidor, SOL_SOCKET, SO_REUSEADDR,
               &reutilizar, sizeof(reutilizar));

    direccion.sin_family = AF_INET;
    direccion.sin_addr.s_addr = htonl(INADDR_ANY);
    direccion.sin_port = htons(HTTP_PUERTO);

    if (bind(servidor, (struct sockaddr *)&direccion,
             sizeof(direccion)) < 0 || listen(servidor, 16) < 0) {
        close(servidor);
        servidor = -1;
        return -1;
    }

    activo = 1;
    if (pthread_create(&hilo, NULL, hilo_http, NULL) != 0) {
        activo = 0;
        close(servidor);
        servidor = -1;
        return -1;
    }

    return 0;
}

void http_detener(void)
{
    if (!activo) return;
    activo = 0;
    pthread_join(hilo, NULL);
    close(servidor);
    servidor = -1;
}