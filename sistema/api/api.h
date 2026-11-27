// ============================================================
// api.h
// Este documento se encarga de definir las estructuras, funciones y parametros del api
///////// Solo es un borrador inicial ///////////
// ============================================================

#ifndef API_H
#define API_H

#include <stddef.h>
#include <stdint.h>

#define API_IP_LONGITUD 46u

/* Deteccion de errores */
typedef enum {
    OK = 0,
    ERROR_CONFIG,
    ERROR_RED,
    ERROR_MEM,
    ERROR_NODE,
    ERROR_NO_INIT,
    ERROR_ARG,
    ERROR_STATS,
    ERROR_STATE,
    ERROR_PAGER
} status_t;


/* Estados MSI publicos de las paginas */
typedef enum {
    PAGE_INVALID = 0,
    PAGE_SHARED,
    PAGE_MODIFIED
} page_state_t;


/* Estadisticas publicas del sistema DSM */
typedef struct {
    uint64_t faults_pagina;
    uint64_t paginas_enviadas;
    uint64_t paginas_recibidas;
    uint64_t invalidadas;

    uint64_t solicitudes_memoria;
    uint64_t liberaciones_memoria;
    uint64_t errores_red;
    uint64_t bytes_enviados;
    uint64_t bytes_recibidos;

    uint64_t uptime_ms;
    uint64_t latencia_promedio_us;
    uint64_t latencia_maxima_us;
    uint64_t latencia_muestras;

    size_t mem_total;
    size_t mem_usada;
} stats_t;


/* Informacion publica de un nodo */
typedef struct {
    uint32_t nodo_id;
    char ip[API_IP_LONGITUD];

    uint16_t puerto_pages;
    uint16_t puerto_nodos;
    uint16_t puerto_heartbeat;

    uint64_t memoria_total;
    uint64_t memoria_libre;

    uint8_t estado;
    uint64_t ultimo_heartbeat_ms;
} node_info_t;


/* INICIALIZACION */

status_t sistema_init(const char *config_path);

void *sistema_alloc(size_t size);

status_t sistema_wait_ready(void);


/* RED: se activa explicitamente despues de sistema_init. */
status_t sistema_red_iniciar(void);
status_t sistema_red_unirse(const char *ip, uint16_t puerto_nodos);
status_t sistema_red_heartbeat(uint32_t nodo_id);
status_t sistema_red_pedir_pagina(uint32_t nodo_id, uint32_t pagina_id);
status_t sistema_red_invalidar_copia(uint32_t nodo_id, uint32_t pagina_id);
status_t sistema_red_verificar_nodos(uint32_t *nuevos_caidos);
status_t sistema_red_detener(void);

/* EJECUCION */

status_t sistema_block_all(void);

status_t sistema_lock(int lock_id);

status_t sistema_unlock(int lock_id);


/* LIBERACION Y FINALIZACION */

status_t sistema_free(void *ptr);

status_t sistema_finalize(void);


/* CONSULTAS DE NODOS */

int sistema_node_id(void);

int sistema_node_cantidad(void);

status_t sistema_node_info(int node_id, node_info_t *out);

void sistema_print_nodes(void);


/* ESTADISTICAS */

status_t sistema_stats(int node_id, stats_t *out);


/* TABLA DE PAGINAS */

void sistema_print_page_table(int node_id);


/* GESTION DEL PAGER */

/* Cambia el estado MSI de una pagina local. */
status_t sistema_page_set_state(
    uint32_t pagina_id,
    page_state_t nuevo_estado
);

/* Consulta el estado MSI de una pagina local. */
/* Indica el origen de una pagina que ya se marco INVALID. */
status_t sistema_page_registrar_origen(uint32_t pagina_id, uint32_t nodo_origen);

status_t sistema_page_get_state(
    uint32_t pagina_id,
    page_state_t *estado
);


/* ERRORES */

status_t sistema_last_error(void);

#endif

