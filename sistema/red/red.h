// ============================================================
// red.h
// Controla hilos de comunicacion y timeouts
// Prototipo 1
// ============================================================

#ifndef DSM_RED_H
#define DSM_RED_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "protocolo.h"

typedef enum {
    RED_OK = 0,
    RED_ERROR_PARAMETROS = -1,
    RED_ERROR_SOCKET = -2,
    RED_ERROR_CONEXION = -3,    // por ejemplo con un nodo caido
    RED_ERROR_TIMEOUT = -4,
    RED_ERROR_CERRADO = -5,     // ha sido bloqueado
    RED_ERROR_PROTOCOLO = -6,   // algo en formato invalido
    RED_ERROR_MEMORIA = -7,
    RED_ERROR_YA_INICIADO = -8
} red_resultado_t;

#define RED_TIMEOUT_LE_MS       3000   // timeout de lectura/escritura en milisegundos
#define RED_TIMEOUT_ESP_MS    500    // espera mensaje

/* Mensaje recibido, ya en orden de host. El mensaje sigue en orden de RED:
 * conviertalo con las funciones proto_*_convertir de protocolo.h. */
typedef struct {
    uint8_t  tipo;
    uint16_t origen;
    uint16_t destino;
    uint32_t id_solicitud;
    uint32_t longitud;
    uint8_t *mensaje;           
} red_mensaje_t;

// vacia el mensaje
void red_mensaje_liberar(red_mensaje_t *msg);

// resultado de mensaje
const char *red_resultado_texto(red_resultado_t r);






//////////////////////// Cliente //////////////////////////////////

// Se conecta, devuelve el file descriptor
red_resultado_t red_conectar(const char *host, uint16_t puerto, int timeout_ms, int *fd_salida);


// envia y recibe mensajes
red_resultado_t red_enviar_mensaje(int fd, uint8_t tipo, uint16_t origen, uint16_t destino, 
                                    uint32_t id_solicitud, const void *mensaje, uint32_t longitud);

// espera timeout para empezar mensaje
red_resultado_t red_recibir_mensaje(int fd, red_mensaje_t *salida, int timeout_inicial_ms, uint32_t max_mensaje);




// solicita y espera respuesta
red_resultado_t red_solicitar(const char *host, uint16_t puerto, uint8_t tipo, uint16_t origen, uint16_t destino,
                              const void *mensaje, uint32_t longitud, red_mensaje_t *respuesta, int timeout_ms);

// solo notifica, no le interesa respuesta
red_resultado_t red_notificar(const char *host, uint16_t puerto, uint8_t tipo, uint16_t origen, uint16_t destino,
                              const void *mensaje, uint32_t longitud, int timeout_ms);

// respuesta usando id de la misma solicitud
red_resultado_t red_responder(int fd, const red_mensaje_t *solicitud, uint8_t tipo, uint16_t mi_id,  
                                const void *mensaje, uint32_t longitud);





//////////////////////// Servidor //////////////////////////////////

// recibe mensaje y responde segun corresponde, identifica el hilo
typedef int (*red_handler_t)(int fd, const red_mensaje_t *msg, void *ctx);

typedef struct {
    int             fd_escucha;
    uint16_t        puerto;
    pthread_t       hilo_aceptar;
    atomic_int      activo;
    atomic_int      conexiones;
    red_handler_t   handler;
    void           *ctx;
    uint32_t        max_mensaje;
    int             iniciado;    
} red_servidor_t;

// abre la red
red_resultado_t red_servidor_iniciar(red_servidor_t *servidor, const char *ip_bind, uint16_t puerto,
                                     red_handler_t handler, void *ctx, uint32_t max_mensaje);

// ya no acepta, solo espera que terminen los hilos
void red_servidor_detener(red_servidor_t *servidor);

#endif