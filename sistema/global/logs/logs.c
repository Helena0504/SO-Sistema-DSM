// ============================================================
// logs.c
// Historial de lo que sucede con el sistema
// Prototipo 1
// ============================================================


#define _POSIX_C_SOURCE 200809L

#include "logs.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static FILE *archivo_log = NULL;
static log_nivel_t nivel_actual = LOG_INFO;

static pthread_mutex_t mutex_logs = PTHREAD_MUTEX_INITIALIZER;

static const char *nombres_nivel[] = {
    "TRACE",
    "DEBUG",
    "INFO",
    "WARNING",
    "ERROR"
};

static int convertir_nivel(const char *nivel, log_nivel_t *salida)
{
    if (nivel == NULL || salida == NULL) {
        return -1;
    }

    for (int i = 0; i <= LOG_ERROR; i++) {
        if (strcmp(nivel, nombres_nivel[i]) == 0) {
            *salida = (log_nivel_t)i;
            return 0;
        }
    }

    return -1;
}

int logs_iniciar(const char *archivo, const char *nivel)
{
    if (archivo == NULL || nivel == NULL) {
        return -1;
    }

    log_nivel_t nuevo_nivel;

    if (convertir_nivel(nivel, &nuevo_nivel) != 0) {
        return -1;
    }

    pthread_mutex_lock(&mutex_logs);

    if (archivo_log != NULL) {
        fclose(archivo_log);
        archivo_log = NULL;
    }

    archivo_log = fopen(archivo, "a");

    if (archivo_log == NULL) {
        pthread_mutex_unlock(&mutex_logs);
        return -1;
    }

    nivel_actual = nuevo_nivel;

    pthread_mutex_unlock(&mutex_logs);

    return 0;
}

void logs_escribir(log_nivel_t nivel, const char *formato, ...)
{
    if (formato == NULL || nivel < LOG_TRACE || nivel > LOG_ERROR) {
        return;
    }

    pthread_mutex_lock(&mutex_logs);

    if (archivo_log == NULL || nivel < nivel_actual) {
        pthread_mutex_unlock(&mutex_logs);
        return;
    }

    time_t ahora = time(NULL);
    struct tm tiempo;

    if (localtime_r(&ahora, &tiempo) == NULL) {
        pthread_mutex_unlock(&mutex_logs);
        return;
    }

    char fecha[32];

    strftime(fecha, sizeof(fecha),
             "%Y-%m-%d %H:%M:%S", &tiempo);

    fprintf(archivo_log, "[%s] [%s] ",
            fecha, nombres_nivel[nivel]);

    va_list argumentos;
    va_start(argumentos, formato);

    vfprintf(archivo_log, formato, argumentos);

    va_end(argumentos);

    fputc('\n', archivo_log);
    fflush(archivo_log);

    pthread_mutex_unlock(&mutex_logs);
}

void logs_cerrar(void)
{
    pthread_mutex_lock(&mutex_logs);

    if (archivo_log != NULL) {
        fflush(archivo_log);
        fclose(archivo_log);
        archivo_log = NULL;
    }

    pthread_mutex_unlock(&mutex_logs);
}