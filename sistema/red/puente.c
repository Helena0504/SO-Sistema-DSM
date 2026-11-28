#define _DEFAULT_SOURCE
#include "puente.h"
#include "../servicios/nodos/nodos.h"
#include "../servicios/asignacion/asig.h"
#include "../servicios/paginador/pager.h"
#include "../servicios/estadisticas/stats.h"

#include <endian.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int mensaje_para_mi(const puente_t *p, const red_mensaje_t *msg)
{
    return msg->destino == p->estado->nodo_id &&
           msg->origen < DSM_MAX_NODOS &&
           msg->origen != p->estado->nodo_id;
}

static int emisor_conocido(const puente_t *p, uint32_t id)
{
    dsm_nodo_t nodo;
    return nodos_obtener(p->estado, id, &nodo) == NODOS_OK &&
           nodo.estado == DSM_NODO_CONECTADO;
}

static void preparar_nodo(const dsm_nodo_t *nodo, msg_nodo_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->nodo_id = htobe32(nodo->nodo_id);
    memcpy(out->ip, nodo->ip, sizeof(out->ip));
    out->puerto_pages = htobe16(nodo->puerto_pages);
    out->puerto_nodos = htobe16(nodo->puerto_nodos);
    out->puerto_heartbeat = htobe16(nodo->puerto_heartbeat);
    out->memoria_total = htobe64(nodo->memoria_total);
    out->memoria_libre = htobe64(nodo->memoria_libre);
}

static int leer_nodo(const msg_nodo_info_t *in, dsm_nodo_t *out)
{
    memset(out, 0, sizeof(*out));
    out->nodo_id = be32toh(in->nodo_id);
    memcpy(out->ip, in->ip, sizeof(out->ip));
    if (memchr(out->ip, '\0', sizeof(out->ip)) == NULL) {
        return -1;
    }
    out->puerto_pages = be16toh(in->puerto_pages);
    out->puerto_nodos = be16toh(in->puerto_nodos);
    out->puerto_heartbeat = be16toh(in->puerto_heartbeat);
    out->memoria_total = be64toh(in->memoria_total);
    out->memoria_libre = be64toh(in->memoria_libre);
    out->estado = DSM_NODO_CONECTADO;
    return out->nodo_id < DSM_MAX_NODOS;
}

static int registrar_remoto(puente_t *p, const dsm_nodo_t *nodo)
{
    dsm_nodo_t existente;
    if (nodos_obtener(p->estado, nodo->nodo_id, &existente) == NODOS_OK) {
        /* Solo se acepta reconectar al mismo nodo, con la misma IP y puertos. */
        if (strcmp(existente.ip, nodo->ip) != 0 ||
            existente.puerto_pages != nodo->puerto_pages ||
            existente.puerto_nodos != nodo->puerto_nodos ||
            existente.puerto_heartbeat != nodo->puerto_heartbeat) {
            return -1;
        }
        if (existente.estado == DSM_NODO_DESCONECTADO &&
            nodos_cambiar_estado(p->estado, nodo->nodo_id,
                                DSM_NODO_CONECTADO) != NODOS_OK) {
            return -1;
        }
        uint64_t ahora;
        return nodos_tiempo_ms(&ahora) == NODOS_OK &&
               nodos_heartbeat_recibido(p->estado, nodo->nodo_id,
                                        ahora) == NODOS_OK ? 0 : -1;
    }
    return nodos_registrar(p->estado, nodo) == NODOS_OK ? 0 : -1;
}

static int responder_error(int fd, const red_mensaje_t *msg,
                          const puente_t *p, uint32_t codigo)
{
    msg_error_t error = {0};
    error.codigo = htobe32(codigo);
    red_responder(fd, msg, ERROR, (uint16_t)p->estado->nodo_id,
                  &error, sizeof(error));
    return 1;
}

static int responder_ok(int fd, const red_mensaje_t *msg,
                       const puente_t *p)
{
    red_responder(fd, msg, RECIBIDO, (uint16_t)p->estado->nodo_id,
                  NULL, 0);
    return 1;
}


/* Los locks y la barrera se coordinan en el nodo 0. El mutex se usa
   solamente para los metadatos, nunca durante operaciones de red. */
static uint64_t mascara_nodos_conectados(puente_t *p)
{
    uint64_t mascara = UINT64_C(1) << p->estado->nodo_id;
    for (uint32_t id = 0; id < DSM_MAX_NODOS; ++id) {
        dsm_nodo_t nodo;
        if (nodos_obtener(p->estado, id, &nodo) == NODOS_OK &&
            nodo.estado == DSM_NODO_CONECTADO) {
            mascara |= UINT64_C(1) << id;
        }
    }
    return mascara;
}

/* Devuelve 1 cuando todos llegaron, 0 si faltan nodos y -1 si
   el mensaje usa una generacion futura. */
static int registrar_llegada(puente_t *p, uint32_t nodo, uint32_t generacion)
{
    if (nodo >= DSM_MAX_NODOS) return -1;
    pthread_mutex_lock(&p->mutex_sincronizacion);
    if (generacion > p->generacion_barrera) {
        pthread_mutex_unlock(&p->mutex_sincronizacion);
        return -1;
    }
    if (generacion == p->generacion_barrera) {
        p->llegadas_barrera |= UINT64_C(1) << nodo;
        uint64_t esperados = mascara_nodos_conectados(p);
        if ((p->llegadas_barrera & esperados) == esperados) {
            p->generacion_barrera++;
            p->llegadas_barrera = 0;
        }
    }
    int terminado = generacion < p->generacion_barrera;
    pthread_mutex_unlock(&p->mutex_sincronizacion);
    return terminado;
}

static int atender_sincronizacion(int fd, const red_mensaje_t *msg, puente_t *p)
{
    if (p->estado->nodo_id != 0 || !mensaje_para_mi(p, msg) ||
        !emisor_conocido(p, msg->origen) ||
        msg->longitud != sizeof(msg_sync_t)) {
        return responder_error(fd, msg, p, PROTOCOL_ERROR_TIPO_INVALIDO);
    }
    msg_sync_t cuerpo;
    memcpy(&cuerpo, msg->mensaje, sizeof(cuerpo));
    uint32_t id = be32toh(cuerpo.id);
    if (msg->tipo == BLOQUEO_TODOS) {
        int listo = registrar_llegada(p, msg->origen, id);
        if (listo < 0) return responder_error(fd, msg, p, PROTOCOL_ERROR_TIPO_INVALIDO);
        if (!listo) return responder_error(fd, msg, p, PROTOCOL_ERROR_OCUPADO);
        red_responder(fd, msg, BLOQUEO_SUELTO, (uint16_t)p->estado->nodo_id,
                      msg->mensaje, msg->longitud);
        return 1;
    }
    if (id >= PUENTE_MAX_LOCKS) {
        return responder_error(fd, msg, p, PROTOCOL_ERROR_TIPO_INVALIDO);
    }
    if (msg->tipo == LOCK_PEDIDO) {
        pthread_mutex_lock(&p->mutex_sincronizacion);
        int logrado = sem_trywait(&p->locks_central[id]) == 0;
        if (logrado) p->propietario_lock[id] = msg->origen;
        pthread_mutex_unlock(&p->mutex_sincronizacion);
        if (!logrado) return responder_error(fd, msg, p, PROTOCOL_ERROR_OCUPADO);
        red_responder(fd, msg, LOCK_ACEPTADO, (uint16_t)p->estado->nodo_id,
                      msg->mensaje, msg->longitud);
        return 1;
    }
    if (msg->tipo == LOCK_SUELTO) {
        pthread_mutex_lock(&p->mutex_sincronizacion);
        int permitido = p->propietario_lock[id] == msg->origen;
        if (permitido) {
            p->propietario_lock[id] = UINT32_MAX;
            sem_post(&p->locks_central[id]);
        }
        pthread_mutex_unlock(&p->mutex_sincronizacion);
        return permitido ? responder_ok(fd, msg, p) :
               responder_error(fd, msg, p, PROTOCOL_ERROR_NO_ES_DUENO);
    }
    return responder_error(fd, msg, p, PROTOCOL_ERROR_TIPO_INVALIDO);
}

static int atender_nodos(int fd, const red_mensaje_t *msg, void *ctx)
{
    puente_t *p = ctx;
    if (msg->tipo == BLOQUEO_TODOS || msg->tipo == LOCK_PEDIDO ||
        msg->tipo == LOCK_SUELTO) {
        return atender_sincronizacion(fd, msg, p);
    }
    if ((msg->tipo == MEM_REQUEST || msg->tipo == MEM_FREE) &&
        mensaje_para_mi(p, msg) && emisor_conocido(p, msg->origen) &&
        msg->longitud == sizeof(msg_memoria_t)) {
        msg_memoria_t solicitud;
        memcpy(&solicitud, msg->mensaje, sizeof(solicitud));
        uint64_t proceso_id = be64toh(solicitud.proceso_id);
        uint32_t inicio = be32toh(solicitud.pagina_inicio);
        uint32_t cantidad = be32toh(solicitud.cantidad_paginas);
        if (proceso_id == 0 || cantidad == 0) {
            return responder_error(fd, msg, p, PROTOCOL_ERROR_PAGINA_INVALIDA);
        }
        pthread_mutex_lock(&p->mutex);
        asig_resultado_t resultado = msg->tipo == MEM_REQUEST
            ? asig_reservar_paginas_remotas(p->estado, proceso_id, inicio, cantidad)
            : asig_liberar_paginas_remotas(p->estado, proceso_id, inicio, cantidad);
        pthread_mutex_unlock(&p->mutex);
        if (resultado != ASIG_OK) {
            return responder_error(fd, msg, p, PROTOCOL_ERROR_SIN_ESPACIO);
        }
        stats_registrar(msg->tipo == MEM_REQUEST ? STATS_SOLICITUD_MEMORIA
                                                  : STATS_LIBERACION_MEMORIA);
        if (msg->tipo == MEM_FREE) return responder_ok(fd, msg, p);
        red_responder(fd, msg, MEM_RESPUESTA, (uint16_t)p->estado->nodo_id,
                      msg->mensaje, msg->longitud);
        return 1;
    }
    if ((msg->destino != PROTOCOL_DESTINO_TODOS &&
         !mensaje_para_mi(p, msg)) || msg->origen >= DSM_MAX_NODOS ||
        msg->origen == p->estado->nodo_id || msg->tipo != JOIN_REQUEST ||
        msg->longitud != sizeof(msg_nodo_info_t)) {
        return responder_error(fd, msg, p, PROTOCOL_ERROR_TIPO_INVALIDO);
    }
    msg_nodo_info_t cuerpo;
    memcpy(&cuerpo, msg->mensaje, sizeof(cuerpo));
    dsm_nodo_t remoto;
    if (leer_nodo(&cuerpo, &remoto) != 1 ||
        remoto.nodo_id != msg->origen ||
        registrar_remoto(p, &remoto) != 0) {
        return responder_error(fd, msg, p, PROTOCOL_ERROR_NODO_DUPLICADO);
    }

    /* De momento la respuesta anuncia el nodo al que nos conectamos. */
    dsm_nodo_t local;
    if (nodos_obtener(p->estado, p->estado->nodo_id, &local) != NODOS_OK) {
        return responder_error(fd, msg, p, PROTOCOL_ERROR_INTERNO);
    }
    uint8_t respuesta[sizeof(msg_node_list_t) + sizeof(msg_nodo_info_t)];
    msg_node_list_t lista = {.cantidad = htobe32(1)};
    msg_nodo_info_t info;
    preparar_nodo(&local, &info);
    memcpy(respuesta, &lista, sizeof(lista));
    memcpy(respuesta + sizeof(lista), &info, sizeof(info));
    red_responder(fd, msg, JOIN_RECIBIDO, (uint16_t)p->estado->nodo_id,
                  respuesta, sizeof(respuesta));
    return 1;
}

static int atender_heartbeat(int fd, const red_mensaje_t *msg, void *ctx)
{
    puente_t *p = ctx;
    if (!mensaje_para_mi(p, msg) || msg->tipo != HEARTBEAT ||
        msg->longitud != sizeof(msg_heartbeat_t) ||
        !emisor_conocido(p, msg->origen)) {
        return responder_error(fd, msg, p, PROTOCOL_ERROR_TIPO_INVALIDO);
    }
    uint64_t ahora;
    if (nodos_tiempo_ms(&ahora) != NODOS_OK ||
        nodos_heartbeat_recibido(p->estado, msg->origen, ahora) != NODOS_OK) {
        return responder_error(fd, msg, p, PROTOCOL_ERROR_INTERNO);
    }
    stats_registrar(STATS_HEARTBEAT_RECIBIDO);
    red_responder(fd, msg, HEART_RECIBIDO, (uint16_t)p->estado->nodo_id,
                  msg->mensaje, msg->longitud);
    return 1;
}

/* El servidor ya mantiene p->mutex al conceder escritura exclusiva. */
static puente_resultado_t invalidar_copia_bloqueada(puente_t *p,
                                                 uint32_t destino,
                                                 uint32_t pagina_id);

static int atender_pagina(int fd, const red_mensaje_t *msg, void *ctx)
{
    puente_t *p = ctx;
    if (!mensaje_para_mi(p, msg) || !emisor_conocido(p, msg->origen)) {
        return responder_error(fd, msg, p, PROTOCOL_ERROR_NODO_DUPLICADO);
    }

    if (msg->tipo == PAGE_REQUEST &&
        msg->longitud == sizeof(msg_page_request_t)) {
        msg_page_request_t pedido;
        memcpy(&pedido, msg->mensaje, sizeof(pedido));
        uint32_t pagina_id = be32toh(pedido.pagina);
        int escritura = pedido.acceso == PROTOCOL_ACCESO_ESCRITURA;
        if ((pedido.acceso != PROTOCOL_ACCESO_LECTURA && !escritura) ||
            pagina_id >= p->estado->numero_paginas) {
            return responder_error(fd, msg, p, PROTOCOL_ERROR_PAGINA_INVALIDA);
        }
        size_t tam = p->estado->tamano_pagina;
        uint8_t *buffer = malloc(sizeof(msg_page_data_t) + tam);
        if (buffer == NULL) {
            return responder_error(fd, msg, p, PROTOCOL_ERROR_SIN_ESPACIO);
        }

        pthread_mutex_lock(&p->mutex);
        pager_resultado_t r = pager_preparar_envio(
            p->estado, pagina_id, msg->origen,
            buffer + sizeof(msg_page_data_t), tam);
        if (r != PAGER_OK) {
            pthread_mutex_unlock(&p->mutex);
            free(buffer);
            return responder_error(fd, msg, p, PROTOCOL_ERROR_PAGINA_INVALIDA);
        }
        if (escritura) {
            /* Conceder exclusividad solo desde el propietario original.
               Las demas copias deben invalidarse antes del permiso de escritura. */
            dsm_pagina_t *pagina = &p->estado->tabla_paginas[pagina_id];
            if (pagina->nodo_origen != p->estado->nodo_id) {
                pthread_mutex_unlock(&p->mutex);
                free(buffer);
                return responder_error(fd, msg, p, PROTOCOL_ERROR_NO_ES_DUENO);
            }
            uint64_t otras = pagina->nodos_con_copia &
                ~((UINT64_C(1) << p->estado->nodo_id) |
                  (UINT64_C(1) << msg->origen));
            /* Una lectura desde el origen del cliente debe estar al dia. */
            for (uint32_t id = 0; id < DSM_MAX_NODOS; id++) {
                if ((otras & (UINT64_C(1) << id)) &&
                    invalidar_copia_bloqueada(p, id, pagina_id) != PUENTE_OK) {
                    pthread_mutex_unlock(&p->mutex);
                    free(buffer);
                    return responder_error(fd, msg, p, PROTOCOL_ERROR_OCUPADO);
                }
            }
            r = pager_cambiar_estado(p->estado, pagina_id, DSM_INVALID);
            if (r == PAGER_OK) {
                pagina->nodo_exclusivo = msg->origen;
                pagina->nodos_con_copia = UINT64_C(1) << msg->origen;
            }
        } else {
            /* Proteger la pagina antes del envio. */
            r = pager_cambiar_estado(p->estado, pagina_id, DSM_SHARED);
            if (r == PAGER_OK) {
                p->estado->tabla_paginas[pagina_id].nodos_con_copia |=
                    UINT64_C(1) << msg->origen;
            }
        }
        if (r != PAGER_OK) {
            pthread_mutex_unlock(&p->mutex);
            free(buffer);
            return responder_error(fd, msg, p, PROTOCOL_ERROR_INTERNO);
        }
        msg_page_data_t encabezado = {0};
        encabezado.pagina = htobe32(pagina_id);
        encabezado.estado_concedido = escritura ? PROTOCOL_ESTADO_MODIFIED
                                                : PROTOCOL_ESTADO_SHARED;
        encabezado.version = htobe32(p->estado->tabla_paginas[pagina_id].version);
        memcpy(buffer, &encabezado, sizeof(encabezado));
        red_resultado_t envio = red_responder(
            fd, msg, PAGE_DATA, (uint16_t)p->estado->nodo_id,
            buffer, (uint32_t)(sizeof(encabezado) + tam));
        free(buffer);
        if (envio != RED_OK) {
            pthread_mutex_unlock(&p->mutex);
            return 1;
        }

        /* Registrar la copia solo tras recibir confirmacion del destino. */
        red_mensaje_t ack;
        red_resultado_t lectura = red_recibir_mensaje(
            fd, &ack, RED_TIMEOUT_LE_MS, sizeof(msg_error_t));
        if (lectura == RED_OK && ack.tipo == RECIBIDO &&
            ack.longitud == 0 && ack.id_solicitud == msg->id_solicitud &&
            ack.origen == msg->origen && ack.destino == p->estado->nodo_id &&
            (escritura || pager_confirmar_copia(p->estado, pagina_id,
                                                msg->origen) == PAGER_OK)) {
            responder_ok(fd, msg, p);
            stats_registrar(STATS_PAGINA_ENVIADA);
            stats_sumar(STATS_BYTES_ENVIADOS, tam);
        } else {
            responder_error(fd, msg, p, PROTOCOL_ERROR_INTERNO);
        }
        if (lectura == RED_OK) {
            red_mensaje_liberar(&ack);
        }
        pthread_mutex_unlock(&p->mutex);
        return 1;
    }

    if (msg->tipo == INVALIDATE && msg->longitud == sizeof(msg_invalidate_t)) {
        msg_invalidate_t invalida;
        memcpy(&invalida, msg->mensaje, sizeof(invalida));
        uint32_t pagina_id = be32toh(invalida.pagina);
        pthread_mutex_lock(&p->mutex);
        int permitido = pagina_id < p->estado->numero_paginas &&
                        p->estado->tabla_paginas[pagina_id].ocupada &&
                        p->estado->tabla_paginas[pagina_id].nodo_origen == msg->origen &&
                        p->estado->tabla_paginas[pagina_id].estado_msi == DSM_SHARED;
        pager_resultado_t r = permitido
            ? pager_cambiar_estado(p->estado, pagina_id, DSM_INVALID)
            : PAGER_ERROR_PARAMETROS;
        pthread_mutex_unlock(&p->mutex);
        if (r != PAGER_OK) {
            return responder_error(fd, msg, p, PROTOCOL_ERROR_PAGINA_INVALIDA);
        }
        stats_registrar(STATS_INVALIDACION_RECIBIDA);
        return responder_ok(fd, msg, p);
    }

    return responder_error(fd, msg, p, PROTOCOL_ERROR_TIPO_INVALIDO);
}

static int atender_consultas(int fd, const red_mensaje_t *msg, void *ctx)
{
    puente_t *p = ctx;
    if (!mensaje_para_mi(p, msg) || !emisor_conocido(p, msg->origen) ||
        msg->longitud != 0) {
        return responder_error(fd, msg, p, PROTOCOL_ERROR_TIPO_INVALIDO);
    }
    if (msg->tipo == QUERY_STATS) {
        stats_snapshot_t snap;
        uint8_t buffer[STATS_SERIAL_BYTES];
        if (stats_obtener(&snap) != STATS_OK ||
            stats_serializar(&snap, buffer, sizeof(buffer)) != STATS_OK) {
            return responder_error(fd, msg, p, PROTOCOL_ERROR_INTERNO);
        }
        red_responder(fd, msg, QUERY_REPLY, (uint16_t)p->estado->nodo_id,
                      buffer, sizeof(buffer));
        return 1;
    }
    if (msg->tipo == QUERY_NODES) {
        dsm_nodo_t local;
        if (nodos_obtener(p->estado, p->estado->nodo_id, &local) != NODOS_OK)
            return responder_error(fd, msg, p, PROTOCOL_ERROR_INTERNO);
        local.memoria_total = p->estado->memoria_total;
        local.memoria_libre = p->estado->memoria_libre;
        msg_nodo_info_t datos;
        preparar_nodo(&local, &datos);
        red_responder(fd, msg, QUERY_REPLY, (uint16_t)p->estado->nodo_id,
                      &datos, sizeof(datos));
        return 1;
    }
    if (msg->tipo == QUERY_PAGES) {
        /* Se envia un contador seguido de registros de longitud fija. */
        pthread_mutex_lock(&p->mutex);
        size_t ocupadas = 0;
        for (uint32_t i = 0; i < p->estado->numero_paginas; ++i)
            if (p->estado->tabla_paginas[i].ocupada) ++ocupadas;
        if (ocupadas > (PROTOCOL_MAX_MENSAJE - sizeof(uint32_t)) /
                        sizeof(msg_pagina_consulta_t)) {
            pthread_mutex_unlock(&p->mutex);
            return responder_error(fd, msg, p, PROTOCOL_ERROR_SIN_ESPACIO);
        }
        size_t bytes = sizeof(uint32_t) +
                       ocupadas * sizeof(msg_pagina_consulta_t);
        uint8_t *buffer = calloc(1, bytes);
        if (buffer == NULL) {
            pthread_mutex_unlock(&p->mutex);
            return responder_error(fd, msg, p, PROTOCOL_ERROR_INTERNO);
        }
        uint32_t cantidad = htobe32((uint32_t)ocupadas);
        memcpy(buffer, &cantidad, sizeof(cantidad));
        msg_pagina_consulta_t *datos = (msg_pagina_consulta_t *)(void *)
                                      (buffer + sizeof(uint32_t));
        size_t indice = 0;
        for (uint32_t i = 0; i < p->estado->numero_paginas; ++i) {
            const dsm_pagina_t *pagina = &p->estado->tabla_paginas[i];
            if (!pagina->ocupada) continue;
            msg_pagina_consulta_t *r = &datos[indice++];
            r->pagina = htobe32(pagina->pagina_id);
            r->nodo_origen = htobe32(pagina->nodo_origen);
            r->nodo_respaldo = htobe32(pagina->nodo_respaldo);
            r->nodo_exclusivo = htobe32(pagina->nodo_exclusivo);
            r->version = htobe32(pagina->version);
            r->estado_msi = pagina->estado_msi;
            r->proceso_id = htobe64(pagina->proceso_id);
            r->nodos_con_copia = htobe64(pagina->nodos_con_copia);
        }
        pthread_mutex_unlock(&p->mutex);
        red_responder(fd, msg, QUERY_REPLY, (uint16_t)p->estado->nodo_id,
                      buffer, (uint32_t)bytes);
        free(buffer);
        return 1;
    }
    return responder_error(fd, msg, p, PROTOCOL_ERROR_TIPO_INVALIDO);
}

puente_resultado_t puente_iniciar(puente_t *p, dsm_estado_t *estado,
                                 const config_t *conf)
{
    if (p == NULL || estado == NULL || conf == NULL ||
        estado->tabla_nodos == NULL || estado->tabla_paginas == NULL ||
        conf->nodo_id != estado->nodo_id ||
        conf->puertoNodos == 0 || conf->puertoHeartbeat == 0 ||
        conf->puertoPages == 0 || conf->puertoConsulta == 0) {
        return PUENTE_ERROR_PARAMETROS;
    }
    if (p->iniciado) {
        return PUENTE_ERROR_INICIADO;
    }
    /* Un puente_t nuevo debe estar inicializado en cero. */
    memset(p, 0, sizeof(*p));
    p->estado = estado;
    p->config = *conf;
    if (pthread_mutex_init(&p->mutex, NULL) != 0) {
        return PUENTE_ERROR_RED;
    }
    if (pthread_mutex_init(&p->mutex_sincronizacion, NULL) != 0) {
        pthread_mutex_destroy(&p->mutex);
        return PUENTE_ERROR_RED;
    }
    uint32_t creados = 0;
    for (; creados < PUENTE_MAX_LOCKS; ++creados) {
        if (sem_init(&p->locks_central[creados], 0, 1) != 0) break;
        p->propietario_lock[creados] = UINT32_MAX;
    }
    if (creados != PUENTE_MAX_LOCKS) {
        for (uint32_t i = 0; i < creados; ++i) sem_destroy(&p->locks_central[i]);
        pthread_mutex_destroy(&p->mutex_sincronizacion);
        pthread_mutex_destroy(&p->mutex);
        return PUENTE_ERROR_RED;
    }

    const char *ip = conf->ip_local;
    if (red_servidor_iniciar(&p->servidor_nodos, ip, conf->puertoNodos,
                            atender_nodos, p, 512) != RED_OK ||
        red_servidor_iniciar(&p->servidor_heartbeat, ip, conf->puertoHeartbeat,
                            atender_heartbeat, p, 512) != RED_OK ||
        red_servidor_iniciar(&p->servidor_paginas, ip, conf->puertoPages,
                            atender_pagina, p, PROTOCOL_MAX_MENSAJE) != RED_OK ||
        red_servidor_iniciar(&p->servidor_consultas, ip, conf->puertoConsulta,
                            atender_consultas, p, STATS_SERIAL_BYTES) != RED_OK) {
        red_servidor_detener(&p->servidor_consultas);
        red_servidor_detener(&p->servidor_paginas);
        red_servidor_detener(&p->servidor_heartbeat);
        red_servidor_detener(&p->servidor_nodos);
        for (uint32_t i = 0; i < PUENTE_MAX_LOCKS; ++i)
            sem_destroy(&p->locks_central[i]);
        pthread_mutex_destroy(&p->mutex_sincronizacion);
        pthread_mutex_destroy(&p->mutex);
        p->estado = NULL;
        return PUENTE_ERROR_RED;
    }
    p->iniciado = 1;
    return PUENTE_OK;
}

void puente_detener(puente_t *p)
{
    if (p == NULL || !p->iniciado) {
        return;
    }
    red_servidor_detener(&p->servidor_consultas);
    red_servidor_detener(&p->servidor_paginas);
    red_servidor_detener(&p->servidor_heartbeat);
    red_servidor_detener(&p->servidor_nodos);
    for (uint32_t i = 0; i < PUENTE_MAX_LOCKS; ++i)
        sem_destroy(&p->locks_central[i]);
    pthread_mutex_destroy(&p->mutex_sincronizacion);
    pthread_mutex_destroy(&p->mutex);
    p->estado = NULL;
    p->iniciado = 0;
}

puente_resultado_t puente_unirse(puente_t *p, const char *ip,
                                uint16_t puerto_nodos)
{
    if (p == NULL || !p->iniciado || ip == NULL || *ip == '\0' ||
        puerto_nodos == 0) {
        return PUENTE_ERROR_PARAMETROS;
    }
    dsm_nodo_t local;
    if (nodos_obtener(p->estado, p->estado->nodo_id, &local) != NODOS_OK) {
        return PUENTE_ERROR_NODO;
    }
    msg_nodo_info_t info;
    preparar_nodo(&local, &info);
    /* Se desconoce el id del destino; el servidor lo valida al recibir. */
    red_mensaje_t respuesta;
    red_resultado_t r = red_solicitar(ip, puerto_nodos, JOIN_REQUEST,
                                    (uint16_t)p->estado->nodo_id,
                                    PROTOCOL_DESTINO_TODOS, &info,
                                    sizeof(info), &respuesta, RED_TIMEOUT_LE_MS);
    if (r != RED_OK) {
        stats_registrar(STATS_ERROR_RED);
        return PUENTE_ERROR_RED;
    }
    /* JOIN_RECIBIDO devuelve una lista con el nodo remoto inicial. */
    puente_resultado_t resultado = PUENTE_ERROR_RESPUESTA;
    if (respuesta.tipo == JOIN_RECIBIDO &&
        respuesta.longitud >= sizeof(msg_node_list_t) + sizeof(msg_nodo_info_t)) {
        msg_node_list_t lista;
        memcpy(&lista, respuesta.mensaje, sizeof(lista));
        if (be32toh(lista.cantidad) == 1 &&
            respuesta.longitud == sizeof(msg_node_list_t) + sizeof(msg_nodo_info_t)) {
            msg_nodo_info_t datos;
            memcpy(&datos, respuesta.mensaje + sizeof(lista), sizeof(datos));
            dsm_nodo_t remoto;
            if (leer_nodo(&datos, &remoto) == 1 &&
                remoto.nodo_id == respuesta.origen &&
                remoto.nodo_id != local.nodo_id &&
                registrar_remoto(p, &remoto) == 0) {
                resultado = PUENTE_OK;
            }
        }
    }
    red_mensaje_liberar(&respuesta);
    return resultado;
}

static puente_resultado_t buscar_remoto(puente_t *p, uint32_t nodo_id,
                                       dsm_nodo_t *out)
{
    if (p == NULL || !p->iniciado || nodo_id == p->estado->nodo_id ||
        nodo_id >= DSM_MAX_NODOS ||
        nodos_obtener(p->estado, nodo_id, out) != NODOS_OK ||
        out->estado != DSM_NODO_CONECTADO) {
        return PUENTE_ERROR_NODO;
    }
    return PUENTE_OK;
}

static int respuesta_valida(const red_mensaje_t *r, uint8_t tipo,
                           uint32_t emisor, uint32_t destino)
{
    return r->tipo == tipo && r->origen == emisor && r->destino == destino;
}

static puente_resultado_t solicitar_memoria(puente_t *p, uint32_t destino,
                     uint64_t proceso_id, uint32_t inicio, uint32_t paginas,
                     uint8_t operacion)
{
    dsm_nodo_t nodo;
    if (buscar_remoto(p, destino, &nodo) != PUENTE_OK ||
        proceso_id == 0 || paginas == 0) return PUENTE_ERROR_NODO;
    msg_memoria_t pedido = {0};
    pedido.proceso_id = htobe64(proceso_id);
    pedido.pagina_inicio = htobe32(inicio);
    pedido.cantidad_paginas = htobe32(paginas);
    red_mensaje_t respuesta;
    red_resultado_t r = red_solicitar(nodo.ip, nodo.puerto_nodos,
                       operacion, (uint16_t)p->estado->nodo_id,
                       (uint16_t)destino, &pedido, sizeof(pedido),
                       &respuesta, RED_TIMEOUT_LE_MS);
    if (r != RED_OK) {
        stats_registrar(STATS_ERROR_RED);
        return PUENTE_ERROR_RED;
    }
    int valido = respuesta_valida(&respuesta,
                       operacion == MEM_REQUEST ? MEM_RESPUESTA : RECIBIDO,
                       destino, p->estado->nodo_id) &&
                 ((operacion == MEM_REQUEST &&
                   respuesta.longitud == sizeof(pedido) &&
                   memcmp(respuesta.mensaje, &pedido, sizeof(pedido)) == 0) ||
                  (operacion == MEM_FREE && respuesta.longitud == 0));
    red_mensaje_liberar(&respuesta);
    return valido ? PUENTE_OK : PUENTE_ERROR_RESPUESTA;
}

puente_resultado_t puente_reservar_memoria(puente_t *p, uint32_t nodo,
                     uint64_t proceso_id, uint32_t inicio, uint32_t paginas)
{
    return solicitar_memoria(p, nodo, proceso_id, inicio, paginas, MEM_REQUEST);
}

puente_resultado_t puente_liberar_memoria(puente_t *p, uint32_t nodo,
                     uint64_t proceso_id, uint32_t inicio, uint32_t paginas)
{
    return solicitar_memoria(p, nodo, proceso_id, inicio, paginas, MEM_FREE);
}

puente_resultado_t puente_enviar_heartbeat(puente_t *p, uint32_t destino)
{
    dsm_nodo_t nodo;
    if (buscar_remoto(p, destino, &nodo) != PUENTE_OK) {
        return PUENTE_ERROR_NODO;
    }
    msg_heartbeat_t hb = {0};
    uint64_t ahora;
    if (nodos_tiempo_ms(&ahora) != NODOS_OK) {
        return PUENTE_ERROR_NODO;
    }
    hb.secuencia = htobe64(ahora);
    hb.tiempo_ms = htobe64(ahora);
    red_mensaje_t respuesta;
    red_resultado_t r = red_solicitar(nodo.ip, nodo.puerto_heartbeat,
                                    HEARTBEAT, (uint16_t)p->estado->nodo_id,
                                    (uint16_t)destino, &hb, sizeof(hb),
                                    &respuesta, RED_TIMEOUT_LE_MS);
    if (r != RED_OK) {
        stats_registrar(STATS_ERROR_RED);
        return PUENTE_ERROR_RED;
    }
    int valido = respuesta_valida(&respuesta, HEART_RECIBIDO, destino,
                                 p->estado->nodo_id) &&
                 respuesta.longitud == sizeof(hb) &&
                 memcmp(respuesta.mensaje, &hb, sizeof(hb)) == 0;
    red_mensaje_liberar(&respuesta);
    if (!valido) {
        return PUENTE_ERROR_RESPUESTA;
    }
    /* Un heartbeat confirmado tambien actualiza el contacto local. */
    if (nodos_heartbeat_recibido(p->estado, destino, ahora) != NODOS_OK) {
        return PUENTE_ERROR_NODO;
    }
    stats_registrar(STATS_HEARTBEAT_ENVIADO);
    return PUENTE_OK;
}

static puente_resultado_t solicitar_pagina(puente_t *p, uint32_t origen,
                                      uint32_t pagina_id, uint8_t acceso)
{
    dsm_nodo_t nodo;
    if (buscar_remoto(p, origen, &nodo) != PUENTE_OK ||
        pagina_id >= p->estado->numero_paginas ||
        !p->estado->tabla_paginas[pagina_id].ocupada ||
        p->estado->tabla_paginas[pagina_id].estado_msi !=
            (acceso == PROTOCOL_ACCESO_ESCRITURA ? DSM_SHARED : DSM_INVALID)) {
        return PUENTE_ERROR_PAGINA;
    }
    int fd;
    if (red_conectar(nodo.ip, nodo.puerto_pages, RED_TIMEOUT_LE_MS, &fd) != RED_OK) {
        stats_registrar(STATS_ERROR_RED);
        return PUENTE_ERROR_RED;
    }
    msg_page_request_t req = {0};
    req.pagina = htobe32(pagina_id);
    req.acceso = acceso;
    uint32_t solicitud_id = 1; /* Una solicitud por conexion en esta version. */
    red_resultado_t r = red_enviar_mensaje(fd, PAGE_REQUEST,
                        (uint16_t)p->estado->nodo_id, (uint16_t)origen,
                        solicitud_id, &req, sizeof(req));
    red_mensaje_t respuesta;
    if (r == RED_OK) {
        r = red_recibir_mensaje(fd, &respuesta, RED_TIMEOUT_LE_MS,
                               PROTOCOL_MAX_MENSAJE);
    }
    puente_resultado_t resultado = PUENTE_ERROR_RED;
    if (r == RED_OK) {
        resultado = PUENTE_ERROR_RESPUESTA;
        size_t tam = p->estado->tamano_pagina;
        if (respuesta_valida(&respuesta, PAGE_DATA, origen,
                             p->estado->nodo_id) &&
            respuesta.id_solicitud == solicitud_id &&
            respuesta.longitud == sizeof(msg_page_data_t) + tam) {
            msg_page_data_t meta;
            memcpy(&meta, respuesta.mensaje, sizeof(meta));
            if (be32toh(meta.pagina) == pagina_id &&
                meta.estado_concedido == (acceso == PROTOCOL_ACCESO_ESCRITURA
                    ? PROTOCOL_ESTADO_MODIFIED : PROTOCOL_ESTADO_SHARED)) {
                pthread_mutex_lock(&p->mutex);
                pager_resultado_t pr = acceso == PROTOCOL_ACCESO_ESCRITURA
                    ? pager_instalar_exclusiva(p->estado, pagina_id, origen,
                        respuesta.mensaje + sizeof(meta), tam,
                        be32toh(meta.version))
                    : pager_instalar_copia(p->estado, pagina_id, origen,
                        respuesta.mensaje + sizeof(meta), tam,
                        be32toh(meta.version));
                pthread_mutex_unlock(&p->mutex);
                if (pr == PAGER_OK) {
                    r = red_enviar_mensaje(fd, RECIBIDO,
                            (uint16_t)p->estado->nodo_id, (uint16_t)origen,
                            solicitud_id, NULL, 0);
                    if (r == RED_OK) {
                        red_mensaje_t final;
                        r = red_recibir_mensaje(fd, &final, RED_TIMEOUT_LE_MS,
                                               sizeof(msg_error_t));
                        if (r == RED_OK) {
                            resultado = respuesta_valida(&final, RECIBIDO,
                                    origen, p->estado->nodo_id) &&
                                    final.id_solicitud == solicitud_id &&
                                    final.longitud == 0
                                    ? PUENTE_OK : PUENTE_ERROR_RESPUESTA;
                            red_mensaje_liberar(&final);
                        } else {
                            resultado = PUENTE_ERROR_RED;
                        }
                    } else {
                        resultado = PUENTE_ERROR_RED;
                    }
                    if (resultado == PUENTE_OK) {
                        stats_registrar(STATS_PAGINA_RECIBIDA);
                        stats_sumar(STATS_BYTES_RECIBIDOS, tam);
                    }
                } else {
                    resultado = PUENTE_ERROR_PAGINA;
                }
            }
        }
        red_mensaje_liberar(&respuesta);
    }
    close(fd);
    if (resultado == PUENTE_ERROR_RED) {
        stats_registrar(STATS_ERROR_RED);
    }
    return resultado;
}

puente_resultado_t puente_pedir_pagina(puente_t *p, uint32_t origen,
                                      uint32_t pagina_id)
{
    return solicitar_pagina(p, origen, pagina_id, PROTOCOL_ACCESO_LECTURA);
}

puente_resultado_t puente_pedir_escritura(puente_t *p, uint32_t origen,
                                         uint32_t pagina_id)
{
    return solicitar_pagina(p, origen, pagina_id, PROTOCOL_ACCESO_ESCRITURA);
}

static puente_resultado_t invalidar_copia_bloqueada(puente_t *p, uint32_t destino,
                                          uint32_t pagina_id)
{
    dsm_nodo_t nodo;
    if (buscar_remoto(p, destino, &nodo) != PUENTE_OK ||
        pagina_id >= p->estado->numero_paginas ||
        !p->estado->tabla_paginas[pagina_id].ocupada ||
        p->estado->tabla_paginas[pagina_id].nodo_origen != p->estado->nodo_id) {
        return PUENTE_ERROR_PAGINA;
    }
    msg_invalidate_t msg = {.pagina = htobe32(pagina_id)};
    red_mensaje_t respuesta;
    red_resultado_t r = red_solicitar(nodo.ip, nodo.puerto_pages,
                                    INVALIDATE, (uint16_t)p->estado->nodo_id,
                                    (uint16_t)destino, &msg, sizeof(msg),
                                    &respuesta, RED_TIMEOUT_LE_MS);
    if (r != RED_OK) {
        stats_registrar(STATS_ERROR_RED);
        return PUENTE_ERROR_RED;
    }
    int valido = respuesta_valida(&respuesta, RECIBIDO, destino,
                                 p->estado->nodo_id) && respuesta.longitud == 0;
    red_mensaje_liberar(&respuesta);
    if (!valido) {
        return PUENTE_ERROR_RESPUESTA;
    }
    pager_resultado_t pr = pager_confirmar_invalidacion(p->estado, pagina_id,
                                                        destino);
    if (pr != PAGER_OK) {
        return PUENTE_ERROR_PAGINA;
    }
    stats_registrar(STATS_INVALIDACION_ENVIADA);
    return PUENTE_OK;
}


puente_resultado_t puente_invalidar_copia(puente_t *p, uint32_t destino,
                                          uint32_t pagina_id)
{
    if (p == NULL || !p->iniciado) return PUENTE_ERROR_PARAMETROS;
    pthread_mutex_lock(&p->mutex);
    puente_resultado_t r = invalidar_copia_bloqueada(p, destino, pagina_id);
    pthread_mutex_unlock(&p->mutex);
    return r;
}

puente_resultado_t puente_verificar_heartbeat(puente_t *p,
                                               uint32_t *nodos_caidos)
{
    if (p == NULL || !p->iniciado || nodos_caidos == NULL) {
        return PUENTE_ERROR_PARAMETROS;
    }
    uint64_t ahora;
    if (nodos_tiempo_ms(&ahora) != NODOS_OK ||
        nodos_verificar_inactivos(p->estado, ahora, p->config.muertoH,
                                nodos_caidos) != NODOS_OK) {
        return PUENTE_ERROR_NODO;
    }
    stats_sumar(STATS_NODO_CAIDO, *nodos_caidos);
    return PUENTE_OK;
}

puente_resultado_t puente_consultar_stats(puente_t *p, uint32_t remoto,
                                         uint16_t puerto_consulta,
                                         stats_snapshot_t *salida)
{
    dsm_nodo_t nodo;
    if (salida == NULL || puerto_consulta == 0 ||
        buscar_remoto(p, remoto, &nodo) != PUENTE_OK) {
        return PUENTE_ERROR_NODO;
    }
    red_mensaje_t respuesta;
    red_resultado_t r = red_solicitar(nodo.ip, puerto_consulta,
                                    QUERY_STATS, (uint16_t)p->estado->nodo_id,
                                    (uint16_t)remoto, NULL, 0, &respuesta,
                                    RED_TIMEOUT_LE_MS);
    if (r != RED_OK) {
        stats_registrar(STATS_ERROR_RED);
        return PUENTE_ERROR_RED;
    }
    puente_resultado_t resultado = PUENTE_ERROR_RESPUESTA;
    if (respuesta_valida(&respuesta, QUERY_REPLY, remoto,
                         p->estado->nodo_id) &&
        stats_deserializar(respuesta.mensaje, respuesta.longitud,
                           salida) == STATS_OK) {
        resultado = PUENTE_OK;
    }
    red_mensaje_liberar(&respuesta);
    return resultado;
}

/* Peticiones al coordinador. Los errores de ocupado se reintentan sin
   mantener mutexes ni sockets abiertos entre intentos. */
static uint64_t tiempo_monotonico_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static void pausa_sincronizacion(void)
{
    struct timespec t = {.tv_sec = 0, .tv_nsec = 20000000};
    nanosleep(&t, NULL);
}

static puente_resultado_t consultar_coordinador(puente_t *p, uint8_t tipo,
                                               uint8_t esperado, uint32_t id)
{
    dsm_nodo_t coordinador;
    if (buscar_remoto(p, 0, &coordinador) != PUENTE_OK) return PUENTE_ERROR_NODO;
    msg_sync_t cuerpo = {.id = htobe32(id)};
    red_mensaje_t respuesta;
    red_resultado_t r = red_solicitar(coordinador.ip, coordinador.puerto_nodos,
               tipo, (uint16_t)p->estado->nodo_id, 0, &cuerpo, sizeof(cuerpo),
               &respuesta, RED_TIMEOUT_LE_MS);
    if (r != RED_OK) return PUENTE_ERROR_RED;
    puente_resultado_t resultado = PUENTE_ERROR_RESPUESTA;
    if (respuesta_valida(&respuesta, esperado, 0, p->estado->nodo_id) &&
        respuesta.longitud == sizeof(cuerpo) &&
        memcmp(respuesta.mensaje, &cuerpo, sizeof(cuerpo)) == 0) {
        resultado = PUENTE_OK;
    } else if (respuesta_valida(&respuesta, RECIBIDO, 0, p->estado->nodo_id) &&
               respuesta.longitud == 0 && tipo == LOCK_SUELTO) {
        resultado = PUENTE_OK;
    } else if (respuesta.tipo == ERROR &&
               respuesta.longitud >= sizeof(uint32_t)) {
        uint32_t codigo;
        memcpy(&codigo, respuesta.mensaje, sizeof(codigo));
        if (be32toh(codigo) == PROTOCOL_ERROR_OCUPADO)
            resultado = PUENTE_ERROR_OCUPADO;
    }
    red_mensaje_liberar(&respuesta);
    return resultado;
}

puente_resultado_t puente_lock_global(puente_t *p, uint32_t lock_id)
{
    if (p == NULL || !p->iniciado || lock_id >= PUENTE_MAX_LOCKS)
        return PUENTE_ERROR_PARAMETROS;
    if (p->estado->nodo_id == 0) {
        while (sem_wait(&p->locks_central[lock_id]) != 0) {
            if (errno != EINTR) return PUENTE_ERROR_RED;
        }
        pthread_mutex_lock(&p->mutex_sincronizacion);
        p->propietario_lock[lock_id] = 0;
        pthread_mutex_unlock(&p->mutex_sincronizacion);
        return PUENTE_OK;
    }
    uint64_t fin = tiempo_monotonico_ms() + 5000u;
    for (;;) {
        puente_resultado_t r = consultar_coordinador(p, LOCK_PEDIDO,
                                                      LOCK_ACEPTADO, lock_id);
        if (r != PUENTE_ERROR_OCUPADO) return r;
        if (tiempo_monotonico_ms() >= fin) return PUENTE_ERROR_OCUPADO;
        pausa_sincronizacion();
    }
}

puente_resultado_t puente_unlock_global(puente_t *p, uint32_t lock_id)
{
    if (p == NULL || !p->iniciado || lock_id >= PUENTE_MAX_LOCKS)
        return PUENTE_ERROR_PARAMETROS;
    if (p->estado->nodo_id != 0)
        return consultar_coordinador(p, LOCK_SUELTO, RECIBIDO, lock_id);
    pthread_mutex_lock(&p->mutex_sincronizacion);
    int valido = p->propietario_lock[lock_id] == 0;
    if (valido) {
        p->propietario_lock[lock_id] = UINT32_MAX;
        sem_post(&p->locks_central[lock_id]);
    }
    pthread_mutex_unlock(&p->mutex_sincronizacion);
    return valido ? PUENTE_OK : PUENTE_ERROR_PARAMETROS;
}

puente_resultado_t puente_barrera_global(puente_t *p)
{
    if (p == NULL || !p->iniciado) return PUENTE_ERROR_PARAMETROS;
    uint32_t generacion = p->generacion_local;
    uint64_t fin = tiempo_monotonico_ms() + 5000u;
    for (;;) {
        puente_resultado_t r;
        if (p->estado->nodo_id == 0) {
            int listo = registrar_llegada(p, 0, generacion);
            r = listo > 0 ? PUENTE_OK :
                (listo == 0 ? PUENTE_ERROR_OCUPADO : PUENTE_ERROR_RESPUESTA);
        } else {
            r = consultar_coordinador(p, BLOQUEO_TODOS,
                                      BLOQUEO_SUELTO, generacion);
        }
        if (r == PUENTE_OK) {
            p->generacion_local++;
            return PUENTE_OK;
        }
        if (r != PUENTE_ERROR_OCUPADO) return r;
        if (tiempo_monotonico_ms() >= fin) return PUENTE_ERROR_OCUPADO;
        pausa_sincronizacion();
    }
}

/* El puerto de consultas puede diferir entre instancias en una misma IP.
   En la configuracion ambos puertos conservan el mismo desplazamiento. */
static uint16_t puerto_consultas_remoto(const puente_t *p, const dsm_nodo_t *n)
{
    int diferencia = (int)p->config.puertoConsulta -
                     (int)p->config.puertoNodos;
    int puerto = (int)n->puerto_nodos + diferencia;
    return puerto > 0 && puerto <= UINT16_MAX ? (uint16_t)puerto : 0;
}

static puente_resultado_t consulta_remota(puente_t *p, uint32_t nodo,
                              uint8_t tipo, red_mensaje_t *respuesta)
{
    dsm_nodo_t remoto;
    if (respuesta == NULL || buscar_remoto(p, nodo, &remoto) != PUENTE_OK)
        return PUENTE_ERROR_NODO;
    uint16_t puerto = puerto_consultas_remoto(p, &remoto);
    if (!puerto) return PUENTE_ERROR_PARAMETROS;
    red_resultado_t resultado = red_solicitar(remoto.ip, puerto,
        tipo, (uint16_t)p->estado->nodo_id, (uint16_t)nodo,
        NULL, 0, respuesta, RED_TIMEOUT_LE_MS);
    if (resultado != RED_OK) return PUENTE_ERROR_RED;
    if (!respuesta_valida(respuesta, QUERY_REPLY, nodo, p->estado->nodo_id)) {
        red_mensaje_liberar(respuesta);
        return PUENTE_ERROR_RESPUESTA;
    }
    return PUENTE_OK;
}

puente_resultado_t puente_consultar_nodo(puente_t *p, uint32_t nodo,
                                         dsm_nodo_t *info)
{
    if (info == NULL) return PUENTE_ERROR_PARAMETROS;
    red_mensaje_t resp = {0};
    puente_resultado_t r = consulta_remota(p, nodo, QUERY_NODES, &resp);
    if (r != PUENTE_OK) return r;
    if (resp.longitud != sizeof(msg_nodo_info_t) ||
        !leer_nodo((const msg_nodo_info_t *)(const void *)resp.mensaje, info) ||
        info->nodo_id != nodo) r = PUENTE_ERROR_RESPUESTA;
    red_mensaje_liberar(&resp);
    return r;
}

puente_resultado_t puente_consultar_paginas(puente_t *p, uint32_t nodo,
                          dsm_pagina_t **paginas, size_t *cantidad)
{
    if (paginas == NULL || cantidad == NULL) return PUENTE_ERROR_PARAMETROS;
    *paginas = NULL;
    *cantidad = 0;
    red_mensaje_t resp = {0};
    puente_resultado_t r = consulta_remota(p, nodo, QUERY_PAGES, &resp);
    if (r != PUENTE_OK) return r;
    if (resp.longitud < sizeof(uint32_t)) {
        r = PUENTE_ERROR_RESPUESTA;
    } else {
        uint32_t cuantos;
        memcpy(&cuantos, resp.mensaje, sizeof(cuantos));
        cuantos = be32toh(cuantos);
        if (cuantos > (PROTOCOL_MAX_MENSAJE - sizeof(uint32_t)) /
                      sizeof(msg_pagina_consulta_t) ||
            resp.longitud != sizeof(uint32_t) +
                            cuantos * sizeof(msg_pagina_consulta_t)) {
            r = PUENTE_ERROR_RESPUESTA;
        } else {
            dsm_pagina_t *datos = calloc(cuantos ? cuantos : 1,
                                         sizeof(*datos));
            if (datos == NULL) r = PUENTE_ERROR_RED;
            else {
                const msg_pagina_consulta_t *orig =
                    (const msg_pagina_consulta_t *)(const void *)
                    (resp.mensaje + sizeof(uint32_t));
                for (uint32_t i = 0; i < cuantos; ++i) {
                    datos[i].pagina_id = be32toh(orig[i].pagina);
                    datos[i].nodo_origen = be32toh(orig[i].nodo_origen);
                    datos[i].nodo_respaldo = be32toh(orig[i].nodo_respaldo);
                    datos[i].nodo_exclusivo = be32toh(orig[i].nodo_exclusivo);
                    datos[i].version = be32toh(orig[i].version);
                    datos[i].estado_msi = orig[i].estado_msi;
                    datos[i].proceso_id = be64toh(orig[i].proceso_id);
                    datos[i].nodos_con_copia = be64toh(orig[i].nodos_con_copia);
                    datos[i].ocupada = 1;
                }
                *paginas = datos;
                *cantidad = cuantos;
            }
        }
    }
    red_mensaje_liberar(&resp);
    return r;
}
