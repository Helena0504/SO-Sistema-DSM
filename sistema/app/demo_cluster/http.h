#ifndef DSM_HTTP_H
#define DSM_HTTP_H
#include "../../global/config.h"

int http_iniciar(const config_t *config);
void http_detener(void);

#endif