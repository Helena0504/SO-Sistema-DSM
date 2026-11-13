// ============================================================
// config.h
// Lee y valida el sistema.conf
// Prototipo 1
// ============================================================


#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

#define CONFIG_MAX_IP 46
#define CONFIG_MAX_ARCHIVO 256

typedef struct {
    /* Nodo */
    uint32_t nodo_id;
    char ip_local[CONFIG_MAX_IP];

    /* Memoria */
    uint32_t pageSize;
    uint64_t memoriaNodo;
    uint64_t direccionBase;

    /* Algoritmos */
    char algoritmoAsignacion[32];
    char algoritmoReemplazo[32];

    /* Red */
    uint16_t puertoBase;
    uint16_t puertoNodos;
    uint16_t puertoHeartbeat;
    uint16_t puertoPages;
    uint16_t puertoConsulta;

    /* Heartbeat */
    uint64_t pausaH;
    uint64_t muertoH;

    /* Logs */
    char logLevel[16];
    char logArchivo[CONFIG_MAX_ARCHIVO];

    /* Extras */
    int ejecucionDistribuida;
    int replicacion;

} config_t;

int config_cargar(const char *ruta, config_t *config);

void config_default(config_t *config);

#endif