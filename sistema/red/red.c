// ============================================================
// red.c
// Controla hilos de comunicacion y timeouts
// Prototipo 1
// ============================================================

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


// esto solo cuenta los ids y asigna el siguiente id
// es atomico para evitar problemas de sincronizacion
static atomic_uint_fast32_t contador_ids = 0;

static uint32_t siguiente_id(void)
{
    return (uint32_t)(atomic_fetch_add(&contador_ids, 1) + 1);
}


// libera el contenido del mensaje y lo deja vacio
void red_mensaje_liberar(red_mensaje_t *msg)
{
    if (msg == NULL) {
        return;
    }
    free(msg->mensaje);
    memset(msg, 0, sizeof(*msg));
}

// Switch para resultados
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

//configurar distintos tipos de sockets
static void configurar_socket(int fd)
{
    //TCP_NODELAY: los mensajes pequennos no tienen mucho timeout
    int uno = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &uno, sizeof(uno));

    //SO_SNDTIMEO: si tiene bastante timeout para que no se combinen accidentalmente los mensajes
    struct timeval tv = { RED_TIMEOUT_LE_MS / 1000, (RED_TIMEOUT_LE_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// TCP es un canal abierto donde fluyen bytes, por eso debe estar escuchando cuando aparece algo
static red_resultado_t esperar_fd(int fd, short evento, int timeout_ms)
{
    // espera a que el socket tenga el evento
    //fd es el socket, event es el evento que esperamos, revents es la respuesta
    struct pollfd p = { .fd = fd, .events = evento, .revents = 0 };
    while(1) {
        //espera a un evento en el socket durante el tiempo maximo de timeout
        //si el nodo murio entonces no se queda pegado infinitamente
        int r = poll(&p, 1, timeout_ms);
        if (r > 0) {
            return RED_OK;
        }
        if (r == 0) {
            return RED_ERROR_TIMEOUT;
        }
        if (errno == EINTR) {
            continue;
        }
        return RED_ERROR_SOCKET;
    }
}

// envia n bytes en buffer por un socket
static red_resultado_t enviar_todo(int fd, const void *buf, size_t n, int flags)
{
    const uint8_t *p = buf;

    // mientras no hayamos enviado todos los bytes, n>0
    while (n > 0) {
        //se usa flag para diferenciar cuando enviamos encabezado o mensaje
        //NOSIGNAL no mata al proceso, permite que haya una respuesta
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL | flags);
        // r es la cantidad de bytes aceptados
        if (r < 0) {
            //reintenta
            if (errno == EINTR) {
                continue;
            }
            //el buffer de envio estuvo lleno mas tiempo que SO_SNDTIMEO
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return RED_ERROR_TIMEOUT;
            }
            // el socket esta cerrado
            if (errno == EPIPE || errno == ECONNRESET) {
                return RED_ERROR_CERRADO;
            }
            //otro error
            return RED_ERROR_SOCKET;
        }
        p += r;
        n -= (size_t)r;
    }
    return RED_OK;
}

// recibe n bytes de un mensaje
static red_resultado_t recibir_todo(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n > 0) {
        red_resultado_t e = esperar_fd(fd, POLLIN, RED_TIMEOUT_LE_MS);
        // hubo un error
        if (e != RED_OK) {
            return e;
        }
        //si r == 0 el otro lado cerro la conexion
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0) {
            return RED_ERROR_CERRADO;
        }
        if (r < 0) {
            //reintenta
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            //el otro nodo se cayo
            if (errno == ECONNRESET) {
                return RED_ERROR_CERRADO;
            }
            //otro error
            return RED_ERROR_SOCKET;
        }
        p += r;
        n -= (size_t)r;
    }
    return RED_OK;
}

// MENSAJES

//envia un mensaje
red_resultado_t red_enviar_mensaje(int fd, uint8_t tipo, uint16_t origen, uint16_t destino, uint32_t id_solicitud,
                                   const void *mensaje, uint32_t longitud)
{
    //validacion formato
    if (fd < 0 || (longitud > 0 && mensaje == NULL) || longitud > PROTOCOL_MAX_MENSAJE) {
        return RED_ERROR_PARAMETROS;
    }

    //encabezado: big endian
    header_t head;
    head.magic        = PROTOCOL_MAGIC;
    head.tipo         = tipo; 
    head.origen       = origen;
    head.destino      = destino;
    head.id_solicitud = id_solicitud;
    head.longitud     = longitud;

    //MSG_MORE avisa que viene el mensaje justo despues
    red_resultado_t e = enviar_todo(fd, &head, sizeof(head), longitud > 0 ? MSG_MORE : 0);
    //hubo error o no hay mensaje que enviar
    if (e != RED_OK || longitud == 0) {
        return e;
    }
    return enviar_todo(fd, mensaje, longitud, 0);
}


//recibe el encabezado y despues el mensaje
red_resultado_t red_recibir_mensaje(int fd, red_mensaje_t *salida, int timeout_inicial_ms, uint32_t max_mensaje)
{
    //validacion de parametros validos
    if (fd < 0 || salida == NULL) {
        return RED_ERROR_PARAMETROS;
    }
    //deja la salida vacia, asi un error no deja basura
    memset(salida, 0, sizeof(*salida));
    //limite del tamanno del mensaje
    if (max_mensaje == 0 || max_mensaje > PROTOCOL_MAX_MENSAJE) {
        max_mensaje = PROTOCOL_MAX_MENSAJE;
    }

    //espera a que empiece un mensaje
    red_resultado_t e = esperar_fd(fd, POLLIN, timeout_inicial_ms);

    //No llego nada en caso de timeout
    if (e != RED_OK) {
        return e;
    }

    header_t head;
    e = recibir_todo(fd, &head, sizeof(head));
    //hubo error
    if (e != RED_OK) {
        return e;
    }
    //no tiene el codigo correcto
    if (be32toh(head.magic) != PROTOCOL_MAGIC) {
        return RED_ERROR_PROTOCOLO;
    }
    uint32_t longitud = be32toh(head.longitud);
    //mensaje demasiado largo
    if (longitud > max_mensaje) {
        return RED_ERROR_PROTOCOLO;
    }


    uint8_t *mensaje = NULL;
    if (longitud > 0) {
        //guarda espacio para mensaje
        mensaje = malloc(longitud);
        if (mensaje == NULL) {
            return RED_ERROR_MEMORIA;
        }
        e = recibir_todo(fd, mensaje, longitud);
        //hubo error libera la memoria
        if (e != RED_OK) {
            free(mensaje);
            return e;
        }
    }

    //mensaje que sale completo, el encabezado ya en orden
    salida->tipo         = head.tipo;
    salida->origen       = head.origen;
    salida->destino      = head.destino;
    salida->id_solicitud = head.id_solicitud;
    salida->longitud     = longitud;
    salida->mensaje      = mensaje;
    return RED_OK;
}

//respuesta a un mensaje: va dirigida a quien envio la solicitud y repite su id
red_resultado_t red_responder(int fd, const red_mensaje_t *solicitud, uint8_t tipo, uint16_t mi_id, const void *mensaje, uint32_t longitud)
{
    if (solicitud == NULL) {
        return RED_ERROR_PARAMETROS;
    }
    return red_enviar_mensaje(fd, tipo, mi_id, solicitud->origen, solicitud->id_solicitud, mensaje, longitud);
}




//////////////////////// Cliente //////////////////////////////////


//primero se conecta a la red
red_resultado_t red_conectar(const char *host, uint16_t puerto, int timeout_ms, int *fd_salida)
{
    //validacion de formato
    if (host == NULL || host[0] == '\0' || puerto == 0 || fd_salida == NULL) {
        return RED_ERROR_PARAMETROS;
    }
    *fd_salida = -1;


    //conexion de puerto e ip
    char servicio[8];
    snprintf(servicio, sizeof(servicio), "%u", (unsigned)puerto);

    //guarda ip o nombre de nodo
    struct addrinfo pistas;
    memset(&pistas, 0, sizeof(pistas));
    pistas.ai_family   = AF_INET;
    pistas.ai_socktype = SOCK_STREAM;

    struct addrinfo *lista = NULL;
    if (getaddrinfo(host, servicio, &pistas, &lista) != 0) {
        return RED_ERROR_CONEXION;
    }


    //si el resultado es un error de conexion
    red_resultado_t resultado = RED_ERROR_CONEXION;
    for (struct addrinfo *a = lista; a != NULL; a = a->ai_next) {
        //se intenta conseguir el socket
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        //si persiste error es un error de socket
        if (fd < 0) {
            resultado = RED_ERROR_SOCKET;
            continue;
        }

        // lo hace no bloqueante
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int r = connect(fd, a->ai_addr, a->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            //espera respuesta con timeout maximo
            if (esperar_fd(fd, POLLOUT, timeout_ms) == RED_OK) {
                //poll avisa que termino y SO_ERROR dice si fue con exito
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                //si hay un error, es -1. sino, es 0
                r = (err == 0) ? 0 : -1;
            } else {
                //timeout
                resultado = RED_ERROR_TIMEOUT;
                r = -1;
            }
        }

        //exito
        if (r == 0) {
            //lo vuelve a hacer bloqueante
            fcntl(fd, F_SETFL, flags);

            //configura el socket con exito
            configurar_socket(fd);
            *fd_salida = fd;
            resultado = RED_OK;
            break;
        }
        close(fd);
    }
    //libera la lista de direcciones
    freeaddrinfo(lista);
    return resultado;
}

//solicita info
red_resultado_t red_solicitar(const char *host, uint16_t puerto, uint8_t tipo, uint16_t origen, uint16_t destino,
                              const void *mensaje, uint32_t longitud, red_mensaje_t *respuesta, int timeout_ms)
{
    if (respuesta == NULL) {
        return RED_ERROR_PARAMETROS;
    }

    //deja la respuesta vacia
    memset(respuesta, 0, sizeof(*respuesta));


    //se conecta al nodo
    int fd = -1;
    red_resultado_t e = red_conectar(host, puerto, timeout_ms, &fd);
    if (e != RED_OK) {
        return e;
    }


    //le envia un mensaje y espera la respuesta
    uint32_t id = siguiente_id();
    e = red_enviar_mensaje(fd, tipo, origen, destino, id, mensaje, longitud);
    if (e == RED_OK) {
        e = red_recibir_mensaje(fd, respuesta, timeout_ms, 0);
    }
    //cierra el socket
    close(fd);

    //la respuesta debe traer el mismo id de la solicitud
    if (e == RED_OK && respuesta->id_solicitud != id) {
        red_mensaje_liberar(respuesta);
        return RED_ERROR_PROTOCOLO;
    }
    return e;
}

red_resultado_t red_notificar(const char *host, uint16_t puerto, uint8_t tipo, uint16_t origen, uint16_t destino,
                              const void *mensaje, uint32_t longitud, int timeout_ms)
{
    //se conecta al nodo
    int fd = -1;
    red_resultado_t e = red_conectar(host, puerto, timeout_ms, &fd);
    if (e != RED_OK) {
        return e;
    }
    //envia y cierra sin esperar respuesta
    e = red_enviar_mensaje(fd, tipo, origen, destino, siguiente_id(), mensaje, longitud);
    close(fd);
    return e;
}

//////////////////////// Servidor //////////////////////////////////

typedef struct {
    red_servidor_t *servidor;
    int             fd;
} conexion_t;


//un hilo atiende una conexion
static void *hilo_conexion(void *arg)
{
    conexion_t *c = arg;
    red_servidor_t *s = c->servidor;
    int fd = c->fd;
    free(c);

    // activo es atomico para sincronizacion
    while (atomic_load(&s->activo)) {
        red_mensaje_t msg;
        //recibe mensaje
        red_resultado_t e = red_recibir_mensaje(fd, &msg, RED_TIMEOUT_ESP_MS, s->max_mensaje);
        //puede que no haya mensaje todavia
        if (e == RED_ERROR_TIMEOUT) {
            continue;
        }
        //cualquier otro error cierra la conexion
        if (e != RED_OK) {
            break;
        }
        int cerrar = s->handler(fd, &msg, s->ctx);
        //libera la memoria del mensaje
        red_mensaje_liberar(&msg);
        if (cerrar != 0) {
            break;
        }
    }

    close(fd);
    //conexiones es atomico para evitar problemas de sincronizacion
    atomic_fetch_sub(&s->conexiones, 1);
    return NULL;
}

//aceptar hilo
static void *hilo_aceptar(void *arg)
{
    red_servidor_t *s = arg;

    while (atomic_load(&s->activo)) {
        //espera a ver si hay conexiones nuevas durante el timeout
        red_resultado_t e = esperar_fd(s->fd_escucha, POLLIN, 200);
        //o no hay conexiones o hay error
        if (e == RED_ERROR_TIMEOUT) {
            continue;
        }
        if (e != RED_OK) {
            break;
        }


        //si hay conexion
        int fd = accept(s->fd_escucha, NULL, NULL);
        //error socket
        if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE) {
                usleep(10000);
            }
            continue;
        }
        //exito socket
        configurar_socket(fd);

        //guarda espacio para conexion
        conexion_t *c = malloc(sizeof(*c));
        if (c == NULL) {
            close(fd);
            continue;
        }
        c->servidor = s;
        c->fd = fd;

        //atomico para sincronizacion
        atomic_fetch_add(&s->conexiones, 1);

        //inicia hilo
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

//inicializa
red_resultado_t red_servidor_iniciar(red_servidor_t *servidor, const char *ip_bind, uint16_t puerto,
                                     red_handler_t handler, void *ctx, uint32_t max_mensaje)
{
    //validaciones
    if (servidor == NULL || puerto == 0 || handler == NULL) {
        return RED_ERROR_PARAMETROS;
    }
    if (servidor->iniciado) {
        return RED_ERROR_YA_INICIADO;
    }

    //error de socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return RED_ERROR_SOCKET;
    }

    //permite reiniciar un nodo
    int uno = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof(uno));

    //direccion y puerto donde se escucha
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
    //error
    if (bind(fd, (struct sockaddr *)&dir, sizeof(dir)) < 0 || listen(fd, 64) < 0) {
        close(fd);
        return RED_ERROR_SOCKET;
    }


    //inicio exitoso, se guarda la informacion
    servidor->fd_escucha  = fd;
    servidor->puerto      = puerto;
    servidor->handler     = handler;
    servidor->ctx         = ctx;
    servidor->max_mensaje = max_mensaje;
    atomic_store(&servidor->activo, 1);
    atomic_store(&servidor->conexiones, 0);

    //error en el hilo del socket
    if (pthread_create(&servidor->hilo_aceptar, NULL, hilo_aceptar, servidor) != 0) {
        close(fd);
        atomic_store(&servidor->activo, 0);
        return RED_ERROR_SOCKET;
    }

    //exito
    servidor->iniciado = 1;
    return RED_OK;
}


//espera a que todos terminen y cierra todo, no acepta mas conexiones
void red_servidor_detener(red_servidor_t *servidor)
{
    if (servidor == NULL || !servidor->iniciado) {
        return;
    }

    atomic_store(&servidor->activo, 0);
    pthread_join(servidor->hilo_aceptar, NULL);
    close(servidor->fd_escucha);

    //cada cierto tiempo revisa si ya terminaron 
    for (int i = 0; i < 500 && atomic_load(&servidor->conexiones) > 0; ++i) {
        usleep(10000);
    }

    //cierra todo
    servidor->fd_escucha = -1;
    servidor->iniciado = 0;
}