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
    PAGER_ERROR_NO_INSTALADO = -7,
    PAGER_ERROR_NODO_NO_DISPONIBLE = -8,
    PAGER_ERROR_COPIA_AUSENTE = -9,
    PAGER_ERROR_BUFFER_PEQUENO = -10
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

/* Copia una pagina local a un buffer para que red.c pueda enviarla.
   No cambia MSI ni registra copias hasta recibir la confirmacion. */
pager_resultado_t pager_preparar_envio(const dsm_estado_t *estado,
                                      uint32_t pagina_id,
                                      uint32_t nodo_destino,
                                      void *buffer,
                                      size_t capacidad);

/* Se llama cuando el destino confirma que recibio la pagina.
   El origen pasa a SHARED y registra la nueva copia. */
pager_resultado_t pager_confirmar_copia(dsm_estado_t *estado,
                                       uint32_t pagina_id,
                                       uint32_t nodo_destino);

/* Quita la copia remota solo despues de confirmar su invalidacion. */
pager_resultado_t pager_confirmar_invalidacion(dsm_estado_t *estado,
                                              uint32_t pagina_id,
                                              uint32_t nodo_destino);

/* Instala una pagina recibida por red sobre una reserva local INVALID.
   La pagina queda SHARED para que no se escriba sin invalidar copias. */
pager_resultado_t pager_instalar_copia(dsm_estado_t *estado,
                                      uint32_t pagina_id,
                                      uint32_t nodo_origen,
                                      const void *contenido,
                                      size_t longitud,
                                      uint32_t version);

/* Instala SIGSEGV para una sola instancia DSM en este proceso.
   Por ahora solo recupera escrituras en paginas SHARED sin copias remotas. */
pager_resultado_t pager_fallos_instalar(dsm_estado_t *estado);

/* Recupera el manejador que estaba instalado antes del DSM. */
pager_resultado_t pager_fallos_desinstalar(void);

#endif
