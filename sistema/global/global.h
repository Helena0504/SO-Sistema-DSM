#ifndef DSM_GLOBAL_H
#define DSM_GLOBAL_H

#include <stdint.h>
#include <stddef.h>

/* Valores iniciales; config.c podra sobrescribirlos al arrancar. */
#define DSM_PAGINA_DEFAULT 4096u
#define DSM_MEMORIA_NODO_DEFAULT 1048576ull
#define DSM_MAX_NODOS 64u
#define DSM_IP_LONGITUD 46u
#define DSM_NODO_SIN_EXCLUSIVIDAD UINT32_MAX

typedef enum {
    DSM_INVALID = 0,
    DSM_SHARED = 1,
    DSM_MODIFIED = 2
} dsm_msi_t;

typedef enum {
    DSM_NODO_DESCONECTADO = 0,
    DSM_NODO_CONECTADO = 1
} dsm_estado_nodo_t;

/* Metadatos de una pagina: el contenido de la pagina se guarda aparte. */
typedef struct {
    uint32_t pagina_id;
    uint32_t nodo_origen;
    uint64_t proceso_id;
    uint32_t nodo_exclusivo;
    uint32_t version;
    uint64_t nodos_con_copia;
    uint8_t estado_msi;
    uint8_t ocupada;
    uint16_t reservado;
    /* Nodo que reservo el respaldo fisico, estable frente a cambios MSI. */
    uint32_t nodo_respaldo;
} dsm_pagina_t;

/* Informacion que cada nodo mantiene sobre los integrantes del DSM. */
typedef struct {
    uint32_t nodo_id;
    char ip[DSM_IP_LONGITUD];
    uint16_t puerto_pages;
    uint16_t puerto_nodos;
    uint16_t puerto_heartbeat;
    uint64_t memoria_total;
    uint64_t memoria_libre;
    uint64_t ultimo_heartbeat_ms;
    uint8_t estado;
    uint8_t reservado[7];
} dsm_nodo_t;

/* Relacion entre un proceso y las paginas de una region asignada. */
typedef struct {
    uint64_t proceso_id;
    uint32_t region_id;
    uint32_t pagina_inicio;
    uint32_t cantidad_paginas;
    uint32_t nodo_solicitante;
    uintptr_t base_virtual;
} dsm_asignacion_t;

/* Estado local de un nodo; los punteros nunca se envian por red. */
typedef struct {
    uint32_t nodo_id;
    uint32_t tamano_pagina;
    uint64_t memoria_total;
    uint64_t memoria_libre;
    uint32_t numero_paginas;
    uint32_t nodos_conocidos;
    dsm_pagina_t *tabla_paginas;
    dsm_nodo_t *tabla_nodos;
    dsm_asignacion_t *tabla_asignaciones;
} dsm_estado_t;

#endif
