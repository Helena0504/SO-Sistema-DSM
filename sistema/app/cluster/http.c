#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include "../../api/api.h"
#include "../../global/config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define HTTP_BUFFER 131072u
#define HTTP_PAGINAS_MOSTRADAS 12u

static int servidor = -1;
static int activo = 0;
static pthread_t hilo;
static const config_t *config_actual = NULL;

static int enviar_todo(int fd, const char *texto)
{
    size_t restante = strlen(texto);
    while (restante > 0) {
        ssize_t enviados = send(fd, texto, restante, MSG_NOSIGNAL);
        if (enviados < 0 && errno == EINTR) continue;
        if (enviados <= 0) return -1;
        texto += enviados;
        restante -= (size_t)enviados;
    }
    return 0;
}

static void agregar(char *html, size_t capacidad, size_t *usados,
                    const char *formato, ...)
{
    if (*usados >= capacidad - 1) return;
    va_list argumentos;
    va_start(argumentos, formato);
    int n = vsnprintf(html + *usados, capacidad - *usados,
                      formato, argumentos);
    va_end(argumentos);
    if (n < 0) return;
    if ((size_t)n >= capacidad - *usados) {
        *usados = capacidad - 1;
    } else {
        *usados += (size_t)n;
    }
}

static const char *estado_msi(page_state_t estado)
{
    switch (estado) {
        case PAGE_INVALID: return "INVALID";
        case PAGE_SHARED: return "SHARED";
        case PAGE_MODIFIED: return "MODIFIED";
        default: return "DESCONOCIDO";
    }
}

static void responder(int cliente)
{
    char solicitud[1024];
    struct timeval espera = {.tv_sec = 2, .tv_usec = 0};
    (void)setsockopt(cliente, SOL_SOCKET, SO_RCVTIMEO,
                     &espera, sizeof(espera));
    ssize_t recibidos = recv(cliente, solicitud, sizeof(solicitud) - 1, 0);
    if (recibidos <= 0) return;
    solicitud[recibidos] = '\0';
    if (strncmp(solicitud, "GET ", 4) != 0) return;

    char *html = calloc(HTTP_BUFFER, 1);
    if (html == NULL) return;
    size_t usados = 0;
    agregar(html, HTTP_BUFFER, &usados,
           "<!DOCTYPE html><html lang=\"es\"><head>"
           "<meta charset=\"UTF-8\"><meta http-equiv=\"refresh\" content=\"4\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>DSM - Monitor</title>"
           "<style>body{font-family:Arial,sans-serif;margin:26px;background:#f6f7f9;color:#243442}"
           "table{border-collapse:collapse;width:100%%;background:#fff;margin-bottom:18px}"
           "th,td{padding:8px;border:1px solid #d7dde3;text-align:left;font-size:13px}"
           "th{background:#263746;color:white}.ok{color:#157144}.down{color:#b3261e}"
           "h1,h2{margin-bottom:10px}.info{color:#586674}"
           "</style></head><body>"
           "<h1>DSM - Estado del cluster</h1>"
           "<p>Nodo local: %u | IP: %s | Conectados: %d/%u</p>"
           "<h2>Consumo y estadisticas por nodo</h2>"
           "<table><tr><th>ID</th><th>IP</th><th>Estado</th>"
           "<th>Memoria usada</th><th>Memoria total</th><th>Fallos</th>"
           "<th>Enviadas</th><th>Recibidas</th><th>Invalidaciones</th></tr>",
           config_actual->nodo_id, config_actual->ip_local,
           sistema_node_cantidad(), config_actual->cantidad_nodos);

    uint64_t memoria_total = 0, memoria_usada = 0;
    uint64_t fallos = 0, enviadas = 0, recibidas_pag = 0, invalidaciones = 0;
    uint32_t consultados = 0;
    for (uint32_t i = 0; i < config_actual->cantidad_nodos; ++i) {
        const config_nodo_t *nodo = &config_actual->nodos[i];
        node_info_t info;
        stats_t stats = {0};
        int conectado = sistema_node_info((int)nodo->id, &info) == OK &&
                        info.estado;
        int tiene_stats = conectado &&
                          sistema_stats((int)nodo->id, &stats) == OK;
        if (tiene_stats) {
            memoria_total += stats.mem_total;
            memoria_usada += stats.mem_usada;
            fallos += stats.faults_pagina;
            enviadas += stats.paginas_enviadas;
            recibidas_pag += stats.paginas_recibidas;
            invalidaciones += stats.invalidadas;
            consultados++;
        }
        agregar(html, HTTP_BUFFER, &usados,
               "<tr><td>%u</td><td>%s</td><td class=\"%s\">%s</td>"
               "<td>%llu</td><td>%llu</td><td>%llu</td>"
               "<td>%llu</td><td>%llu</td><td>%llu</td></tr>",
               nodo->id, nodo->ip, conectado ? "ok" : "down",
               !conectado ? "DESCONECTADO" :
               (tiene_stats ? "CONECTADO" : "SIN RESPUESTA"),
               (unsigned long long)(tiene_stats ? stats.mem_usada : 0),
               (unsigned long long)(tiene_stats ? stats.mem_total : 0),
               (unsigned long long)(tiene_stats ? stats.faults_pagina : 0),
               (unsigned long long)(tiene_stats ? stats.paginas_enviadas : 0),
               (unsigned long long)(tiene_stats ? stats.paginas_recibidas : 0),
               (unsigned long long)(tiene_stats ? stats.invalidadas : 0));
    }
    agregar(html, HTTP_BUFFER, &usados,
           "</table><h2>Resumen global</h2>"
           "<p>Nodos consultados: %u | Memoria total: %llu bytes | "
           "Usada: %llu bytes | Libre: %llu bytes</p>"
           "<p>Fallos: %llu | Paginas enviadas: %llu | Recibidas: %llu | "
           "Invalidaciones: %llu</p>",
           consultados, (unsigned long long)memoria_total,
           (unsigned long long)memoria_usada,
           (unsigned long long)(memoria_total - memoria_usada),
           (unsigned long long)fallos, (unsigned long long)enviadas,
           (unsigned long long)recibidas_pag,
           (unsigned long long)invalidaciones);

    agregar(html, HTTP_BUFFER, &usados, "<h2>Tablas de paginas por nodo</h2>");
    for (uint32_t i = 0; i < config_actual->cantidad_nodos; ++i) {
        uint32_t id = config_actual->nodos[i].id;
        node_info_t info;
        if (sistema_node_info((int)id, &info) != OK || !info.estado)
            continue;
        page_info_t *paginas = NULL;
        size_t cantidad = 0;
        if (sistema_consultar_paginas((int)id, &paginas, &cantidad) != OK) {
            agregar(html, HTTP_BUFFER, &usados,
                   "<p>Nodo %u: no se pudo consultar la tabla.</p>", id);
            continue;
        }
        agregar(html, HTTP_BUFFER, &usados,
               "<h3>Nodo %u (%zu paginas ocupadas)</h3>"
               "<table><tr><th>Pagina</th><th>Proceso</th><th>MSI</th>"
               "<th>Origen</th><th>Respaldo</th><th>Version</th></tr>",
               id, cantidad);
        size_t mostrar = cantidad < HTTP_PAGINAS_MOSTRADAS ?
                         cantidad : HTTP_PAGINAS_MOSTRADAS;
        for (size_t j = 0; j < mostrar; ++j) {
            page_info_t *p = &paginas[j];
            agregar(html, HTTP_BUFFER, &usados,
                   "<tr><td>%u</td><td>%llu</td><td>%s</td>"
                   "<td>%u</td><td>%u</td><td>%u</td></tr>",
                   p->pagina_id, (unsigned long long)p->proceso_id,
                   estado_msi(p->estado_msi), p->nodo_origen,
                   p->nodo_respaldo, p->version);
        }
        agregar(html, HTTP_BUFFER, &usados, "</table>");
        if (cantidad > mostrar)
            agregar(html, HTTP_BUFFER, &usados,
                   "<p class=\"info\">Mostrando %zu de %zu paginas; "
                   "use 'tabla %u' en la terminal para verlas todas.</p>",
                   mostrar, cantidad, id);
        free(paginas);
    }
    agregar(html, HTTP_BUFFER, &usados,
           "<p class=\"info\">Actualizacion cada 4 segundos. "
           "Los datos corresponden a procesos DSM en ejecucion y pueden "
           "cambiar entre consultas.</p></body></html>");

    char cabecera[256];
    snprintf(cabecera, sizeof(cabecera),
             "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n", usados);
    (void)enviar_todo(cliente, cabecera);
    (void)enviar_todo(cliente, html);
    free(html);
}

static void *hilo_http(void *arg)
{
    (void)arg;
    while (activo) {
        struct pollfd evento = {.fd = servidor, .events = POLLIN};
        int resultado = poll(&evento, 1, 500);
        if (resultado <= 0 || !(evento.revents & POLLIN)) continue;
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
    if (config == NULL || (unsigned)config->puertoBase + 5u > UINT16_MAX)
        return -1;
    config_actual = config;
    struct sockaddr_in direccion = {0};
    servidor = socket(AF_INET, SOCK_STREAM, 0);
    if (servidor < 0) return -1;
    int reutilizar = 1;
    setsockopt(servidor, SOL_SOCKET, SO_REUSEADDR,
               &reutilizar, sizeof(reutilizar));
    direccion.sin_family = AF_INET;
    direccion.sin_addr.s_addr = htonl(INADDR_ANY);
    direccion.sin_port = htons((uint16_t)(config->puertoBase + 5u));
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
