#ifndef DSM_PAGER_H
#define DSM_PAGER_H

#include "../../global/global.h"

typedef enum {
    PAGER_OK = 0,
    PAGER_ERROR_PARAMETROS = -1,
    PAGER_ERROR_NO_ASIGNADA = -2,
    PAGER_ERROR_SIN_DATOS = -3,
    PAGER_ERROR_COPIAS_REMOTAS = -4,
    PAGER_ERROR_SISTEMA = -5,
    PAGER_ERROR_YA_INSTALADO = -6,
    PAGER_ERROR_NO_INSTALADO = -7
} pager_resultado_t;

/* Aplica los permisos de Linux segun MSI a una pagina local ya reservada.
   No intercambia mensajes; las invalidaciones remotas quedan para la red. */
pager_resultado_t pager_cambiar_estado(dsm_estado_t *estado,
                                      uint32_t pagina_id,
                                      dsm_msi_t nuevo_estado);

/* Permite consultar el estado local sin exponer los metadatos. */
pager_resultado_t pager_consultar_estado(const dsm_estado_t *estado,
                                        uint32_t pagina_id,
                                        dsm_msi_t *salida);

/* Instala SIGSEGV para una sola instancia DSM en este proceso.
   Por ahora solo recupera escrituras en paginas SHARED sin copias remotas. */
pager_resultado_t pager_fallos_instalar(dsm_estado_t *estado);

/* Recupera el manejador que estaba instalado antes del DSM. */
pager_resultado_t pager_fallos_desinstalar(void);

#endif
