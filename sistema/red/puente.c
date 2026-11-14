#define _DEFAULT_SOURCE
#include "puente.h"
#include "../servicios/nodos/nodos.h"
#include "../servicios/paginador/pager.h"
#include "../servicios/estadisticas/stats.h"

#include <endian.h>
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
        /* No se cambia la identidad del nodo silenciosamente. */
        return strcmp(existente.ip, nodo->ip) == 0 &&
               existente.puerto_pages == nodo->puerto_pages &&
               existente.puerto_nodos == nodo->puerto_nodos &&
               existente.puerto_heartbeat == nodo->puerto_heartbeat &&
               existente.estado == DSM_NODO_CONECTADO ? 0 : -1;
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

static int atender_nodos(int fd, const red_mensaje_t *msg, void *ctx)
{
    puente_t *p = ctx;
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
        if (pedido.acceso != PROTOCOL_ACCESO_LECTURA ||
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
        /* Proteger la pagina antes del envio, no despues del ACK.
           Si la conexion falla, se conserva la copia pendiente para
           evitar que una escritura local deje datos remotos obsoletos. */
        r = pager_cambiar_estado(p->estado, pagina_id, DSM_SHARED);
        if (r != PAGER_OK) {
            pthread_mutex_unlock(&p->mutex);
            free(buffer);
            return responder_error(fd, msg, p, PROTOCOL_ERROR_INTERNO);
        }
        p->estado->tabla_paginas[pagina_id].nodos_con_copia |=
            UINT64_C(1) << msg->origen;
        msg_page_data_t encabezado = {0};
        encabezado.pagina = htobe32(pagina_id);
        encabezado.estado_concedido = PROTOCOL_ESTADO_SHARED;
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
            pager_confirmar_copia(p->estado, pagina_id, msg->origen) == PAGER_OK) {
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
        msg->tipo != QUERY_STATS || msg->longitud != 0) {
        return responder_error(fd, msg, p, PROTOCOL_ERROR_TIPO_INVALIDO);
    }
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
    stats_registrar(STATS_HEARTBEAT_ENVIADO);
    return PUENTE_OK;
}

puente_resultado_t puente_pedir_pagina(puente_t *p, uint32_t origen,
                                      uint32_t pagina_id)
{
    dsm_nodo_t nodo;
    if (buscar_remoto(p, origen, &nodo) != PUENTE_OK ||
        pagina_id >= p->estado->numero_paginas ||
        !p->estado->tabla_paginas[pagina_id].ocupada ||
        p->estado->tabla_paginas[pagina_id].estado_msi != DSM_INVALID) {
        return PUENTE_ERROR_PAGINA;
    }
    int fd;
    if (red_conectar(nodo.ip, nodo.puerto_pages, RED_TIMEOUT_LE_MS, &fd) != RED_OK) {
        stats_registrar(STATS_ERROR_RED);
        return PUENTE_ERROR_RED;
    }
    msg_page_request_t req = {0};
    req.pagina = htobe32(pagina_id);
    req.acceso = PROTOCOL_ACCESO_LECTURA;
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
                meta.estado_concedido == PROTOCOL_ESTADO_SHARED) {
                pthread_mutex_lock(&p->mutex);
                pager_resultado_t pr = pager_instalar_copia(
                    p->estado, pagina_id, origen,
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

puente_resultado_t puente_invalidar_copia(puente_t *p, uint32_t destino,
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
    pthread_mutex_lock(&p->mutex);
    pager_resultado_t pr = pager_confirmar_invalidacion(p->estado, pagina_id,
                                                        destino);
    pthread_mutex_unlock(&p->mutex);
    if (pr != PAGER_OK) {
        return PUENTE_ERROR_PAGINA;
    }
    stats_registrar(STATS_INVALIDACION_ENVIADA);
    return PUENTE_OK;
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
