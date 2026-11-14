// ============================================================
// protocolo.h
// Este documento se encarga de definir el formato de los mensajes entre nodos
// Prototipo 1
// ============================================================

/* Formato
    - Big-Endian 
    - Tamanno fijo
    - Encabezado fijo (17 bytes) + mensaje de 'longitud' bytes */

#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <endian.h>
#include <stdint.h>

#define PROTOCOL_MAGIC        0x44534D31u   // Esto dice en ascii "DSM1" y es el codigo para identificar mensajes de nuestro sistema
#define PROTOCOL_MAX_IP       46   //Tamanno max de una ip escrita como texto
#define PROTOCOL_MAX_TEXTO    64   //Tamanno max del texto de un error

// Limite de tamanno
#define PROTOCOL_MAX_MENSAJE  (4u * 1024u * 1024u)

//Todos los nodos
#define PROTOCOL_DESTINO_TODOS 0xFFFFu  // Mensaje a todos los nodos del sistema, uno por uno desde 0 hasta FFFF

#define PROTOCOL_PACKED __attribute__((packed)) //esto solo siginifica que ponga todo pegado y no meta espacios raros

// TIPOS
// El byte 'tipo' guarda la categoria (4 bits altos) y el subtipo (4 bits bajos)
#define PROTOCOL_TIPO(tipo)     ((unsigned)(tipo) >> 4)     // se saca la categoria corriendo a la derecha
#define PROTOCOL_SUBTIPO(tipo)  ((unsigned)(tipo) & 0x0Fu)  // se saca el subtipo

typedef enum {
    TIPO_NODOS      = 0,
    TIPO_HEARTBEAT  = 1,
    TIPO_PAGINAS    = 2,
    TIPO_MEMORIA    = 3,
    TIPO_CONSULTAS  = 4,
    TIPO_VALIDACION = 5,
    TIPO_ERRORES    = 6
} dsm_categoria_t;


//Mensaje de tipo 0 (nodos)
/*      Subipo              Puerto           Mensaje                     Respuesta
    (1) JOIN_REQUEST      puertoNodos      nodo_info (quien entra)     JOIN_RECIBIDO
    (2) JOIN_RECIBIDO     (respuesta)      node_list                   -

    (3) NODO_ANUNCIO      puertoNodos      nodo_info                   RECIBIDO

    (4) NODO_LEAVE        puertoNodos      vacio                       RECIBIDO

    (5) BLOQUEO_TODOS     puertoNodos      sync                        BLOQUEO_SUELTO
    (6) BLOQUEO_SUELTO    (respuesta)      sync                        -

    (7) LOCK_PEDIDO       puertoNodos      sync                        LOCK_ACEPTADO
    (8) LOCK_ACEPTADO     (respuesta)      sync                        -
    (9) LOCK_SUELTO       puertoNodos      sync                        RECIBIDO
*/

//Mensaje de tipo 1 (heartbeat)
/*       Subipo              Puerto           Mensaje       Respuesta
    (1) HEARTBEAT         puertoHeartbeat    heartbeat      HEART_RECIBIDO
    (2) HEART_RECIBIDO    (respuesta)       heartbeat      -
*/

//Mensaje de tipo 2 (paginas)
/*       Subipo              Puerto           Mensaje                        Respuesta
    (1) PAGE_REQUEST      puertoPages      page_request                   PAGE_DATA | ERROR
    (2) PAGE_DATA         (respuesta)      page_data + bytes de pagina          -
    (3) INVALIDATE        puertoPages      invalidate                     RECIBIDO
    (4) PAGE_EVICT        puertoPages      page_data + bytes de pagina    RECIBIDO | ERROR
 */

//Mensaje de tipo 3 (memoria)
/*       Subipo              Puerto           Mensaje        Respuesta
    (1) MEM_REQUEST       puertoNodos      memoria         MEM_RESPUESTA
    (2) MEM_RESPUESTA     (respuesta)      memoria         -
    (3) MEM_FREE          puertoNodos      memoria         RECIBIDO
 */

//Mensaje de tipo 4 (consultas)
/*       Subipo                         Puerto         Mensaje        Respuesta
(1,2,3) QUERY_STATS/PAGES/NODES    puertoConsulta      vacio      QUERY_REPLY
    (4) QUERY_REPLY                 (respuesta)        texto            -
*/

//Mensaje de tipo 5 (validacion)
/*       Subipo              Puerto       Mensaje       Respuesta
    (1) RECIBIDO          (respuesta)      vacio       -
    (2) NRECIBIDO         (respuesta)      error       -
 */

//Mensaje de tipo 6 (error)
/*       Subipo               Puerto         Mensaje        Respuesta
    (1) ERROR             (respuesta)       error           -
 */



 // Combinacion de tipo+subtipo en un byte
typedef enum {
    /* tipo 0: nodos */
    JOIN_REQUEST    = 0x01,
    JOIN_RECIBIDO   = 0x02,
    NODO_ANUNCIO    = 0x03,
    NODO_LEAVE      = 0x04,
    BLOQUEO_TODOS   = 0x05,
    BLOQUEO_SUELTO  = 0x06,
    LOCK_PEDIDO     = 0x07,
    LOCK_ACEPTADO   = 0x08,
    LOCK_SUELTO     = 0x09,

    /* tipo 1: heartbeat */
    HEARTBEAT       = 0x11,
    HEART_RECIBIDO  = 0x12,

    /* tipo 2: paginas */
    PAGE_REQUEST    = 0x21,
    PAGE_DATA       = 0x22,
    INVALIDATE      = 0x23,
    PAGE_EVICT      = 0x24,

    /* tipo 3: memoria */
    MEM_REQUEST     = 0x31,
    MEM_RESPUESTA   = 0x32,
    MEM_FREE        = 0x33,

    /* tipo 4: consultas */
    QUERY_STATS     = 0x41,
    QUERY_PAGES     = 0x42,
    QUERY_NODES     = 0x43,
    QUERY_REPLY     = 0x44,

    /* tipo 5: validacion */
    RECIBIDO        = 0x51,
    NRECIBIDO       = 0x52,

    /* tipo 6: errores */
    ERROR           = 0x61
} msg_tipo_t;

// cuando una page pide acceso
#define PROTOCOL_ACCESO_LECTURA    1   // osea estado shared
#define PROTOCOL_ACCESO_ESCRITURA  2   // osea estado modified

// Estado MSI de paginas
#define PROTOCOL_ESTADO_INVALID    0
#define PROTOCOL_ESTADO_SHARED     1
#define PROTOCOL_ESTADO_MODIFIED   2

typedef enum {
    PROTOCOL_ERROR_DESCONOCIDO     = 1,
    PROTOCOL_ERROR_TIPO_INVALIDO   = 2,
    PROTOCOL_ERROR_NO_ES_DUENO     = 3,  // no es duenno de la pagina
    PROTOCOL_ERROR_PAGINA_INVALIDA = 4,
    PROTOCOL_ERROR_OCUPADO         = 5,  // no se pudo paginar por un block
    PROTOCOL_ERROR_NODO_DUPLICADO  = 6,
    PROTOCOL_ERROR_INTERNO         = 7,
    PROTOCOL_ERROR_SIN_ESPACIO     = 8   // no hay mas paginas libres
} PROTOCOL_error_t;


//  ENCABEZADO (17 bytes). red.c lo convierte a big-endian al enviar y al recibir
typedef struct PROTOCOL_PACKED {
    uint32_t magic;         // PROTOCOL_MAGIC
    uint8_t  tipo;          // categoria + subtipo
    uint16_t origen;        // id nodo que envia
    uint16_t destino;       // id nodo que recibe 
    uint32_t id_solicitud;  // identificar solicitudes para enviar respuestas
    uint32_t longitud;      // bytes de mensaje
} header_t;


// MENSAJES
// Info de nodo (sirve para diferentes tipos de mensaje)
typedef struct PROTOCOL_PACKED {
    uint32_t nodo_id;
    char     ip[PROTOCOL_MAX_IP];
    uint16_t puerto_pages;
    uint16_t puerto_nodos;
    uint16_t puerto_heartbeat;
    uint64_t memoria_total;
    uint64_t memoria_libre;
} msg_nodo_info_t;

// JOIN_RECIBIDO: 'cantidad' de nodos y luego mensajes de nodos
typedef struct PROTOCOL_PACKED {
    uint32_t cantidad;
} msg_node_list_t;

// HEARTBEAT
typedef struct PROTOCOL_PACKED {
    uint64_t secuencia;
    uint64_t tiempo_ms;      
} msg_heartbeat_t;

// pedir acceso a pagina
typedef struct PROTOCOL_PACKED {
    uint32_t pagina;         // id global, no local
    uint8_t  acceso;
    uint8_t  reservado[3];
} msg_page_request_t;

// estado de pagina y luego los bytes de la pagina
typedef struct PROTOCOL_PACKED {
    uint32_t pagina;
    uint8_t  estado_concedido;   // estado de pagina
    uint8_t  reservado[3];
    uint32_t version;
} msg_page_data_t;


// id de pagina a invalidar
typedef struct PROTOCOL_PACKED {
    uint32_t pagina;
} msg_invalidate_t;


//sirve tanto para pedir memoria como para responder
// en mem_request no se indica pagina de inicio, solo en el mensaje de respuesta
typedef struct PROTOCOL_PACKED {
    uint64_t proceso_id;
    uint32_t region_id;
    uint32_t pagina_inicio;
    uint32_t cantidad_paginas;
} msg_memoria_t;

// para locks
typedef struct PROTOCOL_PACKED {
    uint32_t id;
} msg_sync_t;

// error
typedef struct PROTOCOL_PACKED {
    uint32_t codigo;
    uint32_t pagina;
    char     texto[PROTOCOL_MAX_TEXTO];
} msg_error_t;


// Verifica tamannos maximos de cada parte y mensajes
_Static_assert(sizeof(header_t)           == 17, "header_t cambio de tamanno");
_Static_assert(sizeof(msg_nodo_info_t)    == 72, "msg_nodo_info_t cambio de tamanno");
_Static_assert(sizeof(msg_node_list_t)    == 4,  "msg_node_list_t cambio de tamanno");
_Static_assert(sizeof(msg_heartbeat_t)    == 16, "msg_heartbeat_t cambio de tamanno");
_Static_assert(sizeof(msg_page_request_t) == 8,  "msg_page_request_t cambio de tamanno");
_Static_assert(sizeof(msg_page_data_t)    == 12, "msg_page_data_t cambio de tamanno");
_Static_assert(sizeof(msg_invalidate_t)   == 4,  "msg_invalidate_t cambio de tamanno");
_Static_assert(sizeof(msg_memoria_t)      == 20, "msg_memoria_t cambio de tamanno");
_Static_assert(sizeof(msg_sync_t)         == 4,  "msg_sync_t cambio de tamanno");
_Static_assert(sizeof(msg_error_t)        == 72, "msg_error_t cambio de tamanno");

#endif