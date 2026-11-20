#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

static char *limpiar(char *texto)
{
    while (isspace((unsigned char)*texto)) {
        texto++;
    }

    char *fin = texto + strlen(texto);

    while (fin > texto && isspace((unsigned char)fin[-1])) {
        *--fin = '\0';
    }

    return texto;
}

static int leer_numero(const char *valor, uint64_t *salida)
{
    char *fin;
    errno = 0;

    unsigned long long numero = strtoull(valor, &fin, 0);

    if (errno != 0 || fin == valor || *limpiar(fin) != '\0' ||
        valor[0] == '-') {
        return -1;
    }

    *salida = (uint64_t)numero;
    return 0;
}

static int leer_booleano(const char *valor, int *salida)
{
    if (strcmp(valor, "true") == 0 || strcmp(valor, "1") == 0) {
        *salida = 1;
        return 0;
    }

    if (strcmp(valor, "false") == 0 || strcmp(valor, "0") == 0) {
        *salida = 0;
        return 0;
    }

    return -1;
}

/* Lee entradas id:IP separadas por comas. Rechaza IDs e IPs repetidos. */
static int leer_lista_nodos(char *valor, config_t *config)
{
    if (config->cantidad_nodos != 0) {
        return -1;
    }
    char *cursor = valor;
    while (*cursor != '\0') {
        char *coma = strchr(cursor, ',');
        if (coma != NULL) {
            *coma = '\0';
        }
        char *entrada = limpiar(cursor);
        char *dos_puntos = strchr(entrada, ':');
        if (dos_puntos == NULL || config->cantidad_nodos >= CONFIG_MAX_NODOS) {
            return -1;
        }
        *dos_puntos = '\0';
        uint64_t id;
        char *ip = limpiar(dos_puntos + 1);
        struct in_addr direccion;
        if (leer_numero(limpiar(entrada), &id) != 0 ||
            id >= CONFIG_MAX_NODOS || strlen(ip) >= CONFIG_MAX_IP ||
            inet_pton(AF_INET, ip, &direccion) != 1) {
            return -1;
        }
        for (uint32_t i = 0; i < config->cantidad_nodos; i++) {
            if (config->nodos[i].id == id ||
                strcmp(config->nodos[i].ip, ip) == 0) {
                return -1;
            }
        }
        config_nodo_t *nodo = &config->nodos[config->cantidad_nodos++];
        nodo->id = (uint32_t)id;
        strcpy(nodo->ip, ip);
        if (coma == NULL) {
            break;
        }
        cursor = coma + 1;
        if (*cursor == '\0') {
            return -1;
        }
    }
    return config->cantidad_nodos > 0 ? 0 : -1;
}

/* Busca una IP configurada en las interfaces de esta maquina. */
static int identificar_nodo(config_t *config)
{
    const char *id_prueba = getenv("DSM_NODO_ID");
    if (id_prueba != NULL && *id_prueba != '\0') {
        /* Permite ejecutar varias instancias en una sola PC durante pruebas. */
        uint64_t id;
        if (leer_numero(id_prueba, &id) != 0 || id >= CONFIG_MAX_NODOS) {
            return -1;
        }
        for (uint32_t i = 0; i < config->cantidad_nodos; i++) {
            if (config->nodos[i].id == id) {
                config->nodo_id = (uint32_t)id;
                strcpy(config->ip_local, config->nodos[i].ip);
                return 0;
            }
        }
        return -1;
    }

    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0) {
        return -1;
    }
    int coincidencias = 0;
    for (struct ifaddrs *it = interfaces; it != NULL; it = it->ifa_next) {
        if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        char ip[INET_ADDRSTRLEN];
        struct sockaddr_in *sa = (struct sockaddr_in *)it->ifa_addr;
        if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip)) == NULL) {
            continue;
        }
        for (uint32_t i = 0; i < config->cantidad_nodos; i++) {
            if (strcmp(ip, config->nodos[i].ip) == 0) {
                if (coincidencias == 0) {
                    config->nodo_id = config->nodos[i].id;
                    strcpy(config->ip_local, config->nodos[i].ip);
                }
                coincidencias++;
            }
        }
    }
    freeifaddrs(interfaces);
    /* Una maquina que tenga dos IPs listadas no puede decidir su ID sola. */
    return coincidencias == 1 ? 0 : -1;
}

void config_default(config_t *config)
{
    if (config == NULL) {
        return;
    }

    *config = (config_t){0};

    config->nodo_id = 0;
    strcpy(config->ip_local, "0.0.0.0");

    config->pageSize = 4096;
    config->memoriaNodo = 1048576;
    config->direccionBase = UINT64_C(0x700000000000);

    strcpy(config->algoritmoAsignacion, "ROUND_ROBIN");
    strcpy(config->algoritmoReemplazo, "LRU");

    config->puertoBase = 28000;
    config->puertoNodos = 28001;
    config->puertoHeartbeat = 28002;
    config->puertoPages = 28003;
    config->puertoConsulta = 28004;

    config->pausaH = 1000;
    config->muertoH = 3000;

    strcpy(config->logLevel, "INFO");
    strcpy(config->logArchivo, "dsm_nodo.log");

    config->ejecucionDistribuida = 0;
    config->replicacion = 0;
}

int config_cargar(const char *ruta, config_t *config)
{
    if (ruta == NULL || config == NULL) {
        return -1;
    }

    FILE *archivo = fopen(ruta, "r");

    if (archivo == NULL) {
        return -1;
    }

    config_default(config);

    char linea[8192];
    unsigned numero_linea = 0;
    int resultado = 0;

    while (fgets(linea, sizeof(linea), archivo) != NULL) {
        numero_linea++;

        if (strchr(linea, '\n') == NULL && !feof(archivo)) {
            resultado = -1;
            break;
        }

        char *comentario = strchr(linea, '#');

        if (comentario != NULL) {
            *comentario = '\0';
        }

        char *contenido = limpiar(linea);

        if (*contenido == '\0') {
            continue;
        }

        char *igual = strchr(contenido, '=');

        if (igual == NULL) {
            resultado = -1;
            break;
        }

        *igual = '\0';

        char *clave = limpiar(contenido);
        char *valor = limpiar(igual + 1);

        if (*clave == '\0' || *valor == '\0') {
            resultado = -1;
            break;
        }

        uint64_t numero;

        if (strcmp(clave, "nodoId") == 0) {
            if (leer_numero(valor, &numero) != 0 ||
                numero > UINT32_MAX) {
                resultado = -1;
                break;
            }
            config->nodo_id = (uint32_t)numero;
        }

        else if (strcmp(clave, "ipLocal") == 0) {
            if (strlen(valor) >= sizeof(config->ip_local)) {
                resultado = -1;
                break;
            }
            strcpy(config->ip_local, valor);
        }

        else if (strcmp(clave, "pageSize") == 0) {
            if (leer_numero(valor, &numero) != 0 ||
                numero == 0 || numero > UINT32_MAX) {
                resultado = -1;
                break;
            }
            config->pageSize = (uint32_t)numero;
        }

        else if (strcmp(clave, "memoriaNodo") == 0) {
            if (leer_numero(valor, &numero) != 0 || numero == 0) {
                resultado = -1;
                break;
            }
            config->memoriaNodo = numero;
        }

        else if (strcmp(clave, "direccionBase") == 0) {
            if (leer_numero(valor, &numero) != 0) {
                resultado = -1;
                break;
            }
            config->direccionBase = numero;
        }

        else if (strcmp(clave, "algoritmoAsignacion") == 0) {
            if (strlen(valor) >= sizeof(config->algoritmoAsignacion)) {
                resultado = -1;
                break;
            }
            strcpy(config->algoritmoAsignacion, valor);
        }

        else if (strcmp(clave, "algoritmoReemplazo") == 0) {
            if (strlen(valor) >= sizeof(config->algoritmoReemplazo)) {
                resultado = -1;
                break;
            }
            strcpy(config->algoritmoReemplazo, valor);
        }

        else if (strcmp(clave, "puertoBase") == 0 ||
                 strcmp(clave, "puertoNodos") == 0 ||
                 strcmp(clave, "puertoHeartbeat") == 0 ||
                 strcmp(clave, "puertoPages") == 0 ||
                 strcmp(clave, "puertoConsulta") == 0) {

            if (leer_numero(valor, &numero) != 0 ||
                numero < 28000 || numero > 28099) {
                resultado = -1;
                break;
            }

            uint16_t puerto = (uint16_t)numero;

            if (strcmp(clave, "puertoBase") == 0)
                config->puertoBase = puerto;
            else if (strcmp(clave, "puertoNodos") == 0)
                config->puertoNodos = puerto;
            else if (strcmp(clave, "puertoHeartbeat") == 0)
                config->puertoHeartbeat = puerto;
            else if (strcmp(clave, "puertoPages") == 0)
                config->puertoPages = puerto;
            else
                config->puertoConsulta = puerto;
        }

        else if (strcmp(clave, "pausaH") == 0) {
            if (leer_numero(valor, &numero) != 0 || numero == 0) {
                resultado = -1;
                break;
            }
            config->pausaH = numero;
        }

        else if (strcmp(clave, "muertoH") == 0) {
            if (leer_numero(valor, &numero) != 0 || numero == 0) {
                resultado = -1;
                break;
            }
            config->muertoH = numero;
        }

        else if (strcmp(clave, "logLevel") == 0) {
            if (strlen(valor) >= sizeof(config->logLevel)) {
                resultado = -1;
                break;
            }
            strcpy(config->logLevel, valor);
        }

        else if (strcmp(clave, "logArchivo") == 0) {
            if (strlen(valor) >= sizeof(config->logArchivo)) {
                resultado = -1;
                break;
            }
            strcpy(config->logArchivo, valor);
        }

        else if (strcmp(clave, "ejecucionDistribuida") == 0) {
            if (leer_booleano(valor, &config->ejecucionDistribuida) != 0) {
                resultado = -1;
                break;
            }
        }

        else if (strcmp(clave, "replicacion") == 0) {
            if (leer_booleano(valor, &config->replicacion) != 0) {
                resultado = -1;
                break;
            }
        }

        else if (strcmp(clave, "nodos") == 0) {
            if (leer_lista_nodos(valor, config) != 0) {
                resultado = -1;
                break;
            }
        }
    }

    if (ferror(archivo)) {
        resultado = -1;
    }

    fclose(archivo);

    if (resultado != 0) {
        fprintf(stderr, "Error en sistema.conf, linea %u\n",
                numero_linea);
        return -1;
    }

    if (config->cantidad_nodos != 0 && identificar_nodo(config) != 0) {
        fprintf(stderr, "Ninguna IP de nodos= identifica esta maquina. "
                        "Verifique ip -4 addr y sistema.conf\n");
        return -1;
    }

    if (config->nodo_id >= 64 ||
        config->memoriaNodo % config->pageSize != 0 ||
        config->muertoH < config->pausaH ||
        (strcmp(config->logLevel, "TRACE") != 0 &&
         strcmp(config->logLevel, "DEBUG") != 0 &&
         strcmp(config->logLevel, "INFO") != 0 &&
         strcmp(config->logLevel, "WARNING") != 0 &&
         strcmp(config->logLevel, "ERROR") != 0)) {
        return -1;
    }

    uint16_t puertos[] = {
        config->puertoBase,
        config->puertoNodos,
        config->puertoHeartbeat,
        config->puertoPages,
        config->puertoConsulta
    };

    for (size_t i = 0; i < sizeof(puertos) / sizeof(puertos[0]); i++) {
        for (size_t j = i + 1; j < sizeof(puertos) / sizeof(puertos[0]); j++) {
            if (puertos[i] == puertos[j]) {
                return -1;
            }
        }
    }

    return 0;
}