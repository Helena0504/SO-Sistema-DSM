// ============================================================
// logs.h
// Historial de lo que sucede con el sistema
// Prototipo 1
// ============================================================

#ifndef LOGS_H
#define LOGS_H

typedef enum {
    LOG_TRACE = 0,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR
} log_nivel_t;

int logs_iniciar(const char *archivo, const char *nivel);

void logs_escribir(log_nivel_t nivel, const char *formato, ...);

void logs_cerrar(void);

#endif