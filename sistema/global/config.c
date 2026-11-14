#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

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

    char linea[512];
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

        /*
         * Las claves que todavia no utiliza esta version
         * se ignoran para permitir ampliar el archivo.
         */
        else if (strcmp(clave, "nodos") == 0) {
            continue;
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