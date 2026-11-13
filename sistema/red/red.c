#define _GNU_SOURCE
#include "red.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ------------------------------------------------------------
 * Utilidades internas
 * ------------------------------------------------------------ */

static atomic_uint_fast32_t contador_ids = 0;

static uint32_t siguiente_id(void)
{
    return (uint32_t)(atomic_fetch_add(&contador_ids, 1) + 1);
}

void red_mensaje_liberar(red_mensaje_t *msg)
{
    if (msg == NULL) {
        return;
    }
    free(msg->payload);
    memset(msg, 0, sizeof(*msg));
}

const char *red_resultado_texto(red_resultado_t r)
{
    switch (r) {
    case RED_OK:                  return "ok";
    case RED_ERROR_PARAMETROS:    return "parametros invalidos";
    case RED_ERROR_SOCKET:        return "error de socket";
    case RED_ERROR_CONEXION:      return "no se pudo conectar";
    case RED_ERROR_TIMEOUT:       return "timeout";
    case RED_ERROR_CERRADO:       return "conexion cerrada";
    case RED_ERROR_PROTOCOLO:     return "violacion de protocolo";
    case RED_ERROR_MEMORIA:       return "sin memoria";
    case RED_ERROR_YA_INICIADO:   return "servidor ya iniciado";
    }
    return "desconocido";
}

/* TCP_NODELAY: mensajes pequenos (ACK, INVALIDATE) no deben esperar a Nagle.
 * SO_SNDTIMEO: un send() contra un par detenido no debe colgar para siempre. */
static void configurar_socket(int fd)
{
    int uno = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &uno, sizeof(uno));

    struct timeval tv = { RED_TIMEOUT_IO_MS / 1000,
                          (RED_TIMEOUT_IO_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* Espera a que fd tenga el evento. RED_OK = listo, RED_ERROR_TIMEOUT = no llego. */
static red_resultado_t esperar_fd(int fd, short evento, int timeout_ms)
{
    struct pollfd p = { .fd = fd, .events = evento, .revents = 0 };
    for (;;) {
        int r = poll(&p, 1, timeout_ms);
        if (r > 0) {
            return RED_OK;      /* incluye POLLHUP/POLLERR: recv/send lo reportaran */
        }
        if (r == 0) {
            return RED_ERROR_TIMEOUT;
        }
        if (errno == EINTR) {   /* una senal (p.ej. SIGSEGV manejado) nos interrumpio */
            continue;
        }
        return RED_ERROR_SOCKET;
    }
}

/* TCP es un FLUJO de bytes: send/recv pueden mover menos de lo pedido.
 * Estos bucles son lo que convierte "bytes" en "mensajes completos". */
static red_resultado_t enviar_todo(int fd, const void *buf, size_t n, int flags)
{
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL | flags);  /* sin SIGPIPE */
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return RED_ERROR_TIMEOUT;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                return RED_ERROR_CERRADO;
            }
            return RED_ERROR_SOCKET;
        }
        p += r;
        n -= (size_t)r;
    }
    return RED_OK;
}

static red_resultado_t recibir_todo(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n > 0) {
        red_resultado_t e = esperar_fd(fd, POLLIN, RED_TIMEOUT_IO_MS);
        if (e != RED_OK) {
            return e;
        }
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0) {
            return RED_ERROR_CERRADO;
        }
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (errno == ECONNRESET) {
                return RED_ERROR_CERRADO;
            }
            return RED_ERROR_SOCKET;
        }
        p += r;
        n -= (size_t)r;
    }
    return RED_OK;
}

/* ------------------------------------------------------------
 * Mensajes
 * ------------------------------------------------------------ */

red_resultado_t red_enviar_mensaje(int fd, uint8_t tipo,
                                   uint16_t origen, uint16_t destino,
                                   uint32_t id_solicitud,
                                   const void *payload, uint32_t longitud)
{
    if (fd < 0 || (longitud > 0 && payload == NULL) ||
        longitud > DSM_PROTO_MAX_PAYLOAD) {
        return RED_ERROR_PARAMETROS;
    }

    dsm_wire_header_t h;
    h.magic        = htobe32(DSM_PROTO_MAGIC);
    h.version      = DSM_PROTO_VERSION;
    h.tipo         = tipo;                  /* 1 byte: sin conversion */
    h.origen       = htobe16(origen);
    h.destino      = htobe16(destino);
    h.flags        = 0;
    h.reservado    = 0;
    h.id_solicitud = htobe32(id_solicitud);
    h.longitud     = htobe32(longitud);

    /* MSG_MORE le dice al kernel "viene mas": encabezado y payload salen juntos. */
    red_resultado_t e = enviar_todo(fd, &h, sizeof(h), longitud > 0 ? MSG_MORE : 0);
    if (e != RED_OK || longitud == 0) {
        return e;
    }
    return enviar_todo(fd, payload, longitud, 0);
}

red_resultado_t red_recibir_mensaje(int fd, red_mensaje_t *salida,
                                    int timeout_inicial_ms,
                                    uint32_t max_payload)
{
    if (fd < 0 || salida == NULL) {
        return RED_ERROR_PARAMETROS;
    }
    memset(salida, 0, sizeof(*salida));
    if (max_payload == 0 || max_payload > DSM_PROTO_MAX_PAYLOAD) {
        max_payload = DSM_PROTO_MAX_PAYLOAD;
    }

    red_resultado_t e = esperar_fd(fd, POLLIN, timeout_inicial_ms);
    if (e != RED_OK) {
        return e;       /* RED_ERROR_TIMEOUT aqui es "no hay mensaje", flujo intacto */
    }

    dsm_wire_header_t h;
    e = recibir_todo(fd, &h, sizeof(h));
    if (e != RED_OK) {
        return e;
    }
    if (be32toh(h.magic) != DSM_PROTO_MAGIC || h.version != DSM_PROTO_VERSION) {
        return RED_ERROR_PROTOCOLO;
    }
    uint32_t longitud = be32toh(h.longitud);
    if (longitud > max_payload) {
        return RED_ERROR_PROTOCOLO;     /* nunca reservar segun un valor ajeno sin tope */
    }

    uint8_t *payload = NULL;
    if (longitud > 0) {
        payload = malloc(longitud);
        if (payload == NULL) {
            return RED_ERROR_MEMORIA;
        }
        e = recibir_todo(fd, payload, longitud);
        if (e != RED_OK) {
            free(payload);
            return e;
        }
    }

    salida->tipo         = h.tipo;
    salida->origen       = be16toh(h.origen);
    salida->destino      = be16toh(h.destino);
    salida->id_solicitud = be32toh(h.id_solicitud);
    salida->longitud     = longitud;
    salida->payload      = payload;
    return RED_OK;
}

red_resultado_t red_responder(int fd, const red_mensaje_t *solicitud,
                              uint8_t tipo, uint16_t mi_id,
                              const void *payload, uint32_t longitud)
{
    if (solicitud == NULL) {
        return RED_ERROR_PARAMETROS;
    }
    return red_enviar_mensaje(fd, tipo, mi_id, solicitud->origen,
                              solicitud->id_solicitud, payload, longitud);
}

/* ------------------------------------------------------------
 * Cliente
 * ------------------------------------------------------------ */

red_resultado_t red_conectar(const char *host, uint16_t puerto,
                             int timeout_ms, int *fd_salida)
{
    if (host == NULL || host[0] == '\0' || puerto == 0 || fd_salida == NULL) {
        return RED_ERROR_PARAMETROS;
    }
    *fd_salida = -1;

    char servicio[8];
    snprintf(servicio, sizeof(servicio), "%u", (unsigned)puerto);

    struct addrinfo pistas;
    memset(&pistas, 0, sizeof(pistas));
    pistas.ai_family   = AF_INET;       /* IPv4; acepta IP o nombre de maquina */
    pistas.ai_socktype = SOCK_STREAM;

    struct addrinfo *lista = NULL;
    if (getaddrinfo(host, servicio, &pistas, &lista) != 0) {
        return RED_ERROR_CONEXION;
    }

    red_resultado_t resultado = RED_ERROR_CONEXION;
    for (struct addrinfo *a = lista; a != NULL; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            resultado = RED_ERROR_SOCKET;
            continue;
        }

        /* connect() no bloqueante + poll => timeout propio (el del kernel dura ~2 min). */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int r = connect(fd, a->ai_addr, a->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            if (esperar_fd(fd, POLLOUT, timeout_ms) == RED_OK) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                r = (err == 0) ? 0 : -1;
            } else {
                resultado = RED_ERROR_TIMEOUT;
                r = -1;
            }
        }

        if (r == 0) {
            fcntl(fd, F_SETFL, flags);          /* de vuelta a bloqueante */
            configurar_socket(fd);
            *fd_salida = fd;
            resultado = RED_OK;
            break;
        }
        close(fd);
    }
    freeaddrinfo(lista);
    return resultado;
}

red_resultado_t red_solicitar(const char *host, uint16_t puerto,
                              uint8_t tipo, uint16_t origen, uint16_t destino,
                              const void *payload, uint32_t longitud,
                              red_mensaje_t *respuesta, int timeout_ms)
{
    if (respuesta == NULL) {
        return RED_ERROR_PARAMETROS;
    }
    memset(respuesta, 0, sizeof(*respuesta));

    int fd = -1;
    red_resultado_t e = red_conectar(host, puerto, timeout_ms, &fd);
    if (e != RED_OK) {
        return e;
    }

    uint32_t id = siguiente_id();
    e = red_enviar_mensaje(fd, tipo, origen, destino, id, payload, longitud);
    if (e == RED_OK) {
        e = red_recibir_mensaje(fd, respuesta, timeout_ms, 0);
    }
    close(fd);

    if (e == RED_OK && respuesta->id_solicitud != id) {
        red_mensaje_liberar(respuesta);
        return RED_ERROR_PROTOCOLO;
    }
    return e;
}

red_resultado_t red_notificar(const char *host, uint16_t puerto,
                              uint8_t tipo, uint16_t origen, uint16_t destino,
                              const void *payload, uint32_t longitud,
                              int timeout_ms)
{
    int fd = -1;
    red_resultado_t e = red_conectar(host, puerto, timeout_ms, &fd);
    if (e != RED_OK) {
        return e;
    }
    e = red_enviar_mensaje(fd, tipo, origen, destino, siguiente_id(),
                           payload, longitud);
    close(fd);
    return e;
}

/* ------------------------------------------------------------
 * Servidor: un hilo acepta, un hilo por conexion atiende.
 * ------------------------------------------------------------ */

typedef struct {
    red_servidor_t *servidor;
    int             fd;
} conexion_t;

static void *hilo_conexion(void *arg)
{
    conexion_t *c = arg;
    red_servidor_t *s = c->servidor;
    int fd = c->fd;
    free(c);

    while (atomic_load(&s->activo)) {
        red_mensaje_t msg;
        red_resultado_t e = red_recibir_mensaje(fd, &msg, RED_TIMEOUT_IDLE_MS,
                                                s->max_payload);
        if (e == RED_ERROR_TIMEOUT) {
            continue;           /* nada nuevo: revisar si hay que apagar */
        }
        if (e != RED_OK) {
            break;              /* cerrada, error o violacion de protocolo */
        }
        int cerrar = s->handler(fd, &msg, s->ctx);
        red_mensaje_liberar(&msg);
        if (cerrar != 0) {
            break;
        }
    }

    close(fd);
    atomic_fetch_sub(&s->conexiones, 1);
    return NULL;
}

static void *hilo_aceptar(void *arg)
{
    red_servidor_t *s = arg;

    while (atomic_load(&s->activo)) {
        /* poll con timeout: asi detener() no depende de que llegue otra conexion. */
        red_resultado_t e = esperar_fd(s->fd_escucha, POLLIN, 200);
        if (e == RED_ERROR_TIMEOUT) {
            continue;
        }
        if (e != RED_OK) {
            break;
        }

        int fd = accept(s->fd_escucha, NULL, NULL);
        if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE) {
                usleep(10000);  /* sin descriptores: no girar al 100% de CPU */
            }
            continue;
        }
        configurar_socket(fd);

        conexion_t *c = malloc(sizeof(*c));
        if (c == NULL) {
            close(fd);
            continue;
        }
        c->servidor = s;
        c->fd = fd;

        atomic_fetch_add(&s->conexiones, 1);
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, hilo_conexion, c) != 0) {
            atomic_fetch_sub(&s->conexiones, 1);
            close(fd);
            free(c);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

red_resultado_t red_servidor_iniciar(red_servidor_t *servidor,
                                     const char *ip_bind, uint16_t puerto,
                                     red_handler_t handler, void *ctx,
                                     uint32_t max_payload)
{
    if (servidor == NULL || puerto == 0 || handler == NULL) {
        return RED_ERROR_PARAMETROS;
    }
    if (servidor->iniciado) {
        return RED_ERROR_YA_INICIADO;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return RED_ERROR_SOCKET;
    }

    /* Sin SO_REUSEADDR, reiniciar un nodo falla con "Address already in use"
     * hasta que expiran los TIME_WAIT. */
    int uno = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof(uno));

    struct sockaddr_in dir;
    memset(&dir, 0, sizeof(dir));
    dir.sin_family = AF_INET;
    dir.sin_port   = htons(puerto);
    if (ip_bind == NULL || ip_bind[0] == '\0') {
        dir.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, ip_bind, &dir.sin_addr) != 1) {
        close(fd);
        return RED_ERROR_PARAMETROS;
    }

    if (bind(fd, (struct sockaddr *)&dir, sizeof(dir)) < 0 || listen(fd, 64) < 0) {
        close(fd);
        return RED_ERROR_SOCKET;
    }

    servidor->fd_escucha  = fd;
    servidor->puerto      = puerto;
    servidor->handler     = handler;
    servidor->ctx         = ctx;
    servidor->max_payload = max_payload;
    atomic_store(&servidor->activo, 1);
    atomic_store(&servidor->conexiones, 0);

    if (pthread_create(&servidor->hilo_aceptar, NULL, hilo_aceptar, servidor) != 0) {
        close(fd);
        atomic_store(&servidor->activo, 0);
        return RED_ERROR_SOCKET;
    }
    servidor->iniciado = 1;
    return RED_OK;
}

void red_servidor_detener(red_servidor_t *servidor)
{
    if (servidor == NULL || !servidor->iniciado) {
        return;
    }

    atomic_store(&servidor->activo, 0);
    pthread_join(servidor->hilo_aceptar, NULL);
    close(servidor->fd_escucha);

    /* Los hilos de conexion (detached) revisan 'activo' cada RED_TIMEOUT_IDLE_MS.
     * Espera acotada: un handler bloqueado en otra llamada de red no nos cuelga. */
    for (int i = 0; i < 500 && atomic_load(&servidor->conexiones) > 0; ++i) {
        usleep(10000);
    }

    servidor->fd_escucha = -1;
    servidor->iniciado = 0;
}
