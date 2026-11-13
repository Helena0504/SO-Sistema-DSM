// ============================================================
// stats.h
// Contadores de estadisticas de un nodo del sistema DSM.
// Prototipo 1
// ============================================================

#ifndef STATS_H
#define STATS_H

#include <stdint.h>
#include <stdio.h>

typedef enum {
    STATS_OK = 0,
    STATS_ERROR_PARAMETROS,
    STATS_ERROR_SIN_INICIALIZAR,
    STATS_ERROR_YA_INICIALIZADO,
    STATS_ERROR_BUFFER,        
    STATS_ERROR_RELOJ
} stats_resultado_t;

// Eventos que se pueden contar con contadores
typedef enum {
    STATS_FALLO_LECTURA = 0,     // SIGSEGV por lectura de pagina INVALID
    STATS_FALLO_ESCRITURA,       // SIGSEGV por escritura 
    STATS_PAGINA_ENVIADA,        // PAGE_DATA enviado a otro nodo
    STATS_PAGINA_RECIBIDA,       // PAGE_DATA recibido de otro nodo
    STATS_INVALIDACION_ENVIADA,  // INVALIDATE enviado
    STATS_INVALIDACION_RECIBIDA, // INVALIDATE recibido 
    STATS_SOLICITUD_MEMORIA,     // sistema_alloc exitoso
    STATS_LIBERACION_MEMORIA,    // sistema_free exitoso
    STATS_HEARTBEAT_ENVIADO,
    STATS_HEARTBEAT_RECIBIDO,
    STATS_NODO_CAIDO,            // nodo marcado como desconectado
    STATS_ERROR_RED,             // timeouts, conexiones cerradas, etc.
    STATS_BYTES_ENVIADOS,       
    STATS_BYTES_RECIBIDOS,
    STATS_EVENTO_CANTIDAD       
} stats_evento_t;

// Copia de los contadores que se puede leer sin locks
typedef struct {
    uint64_t contadores[STATS_EVENTO_CANTIDAD];
    uint64_t latencia_total_us;   // suma de la duracion de todos los fallos
    uint64_t latencia_max_us;     // el fallo mas lento
    uint64_t latencia_muestras;   // cuantos fallos se midieron
    uint64_t uptime_ms;           // tiempo desde stats_inicializar
} stats_snapshot_t;

// Tamano fijo del mensaje
#define STATS_SERIAL_PALABRAS (STATS_EVENTO_CANTIDAD + 4)
#define STATS_SERIAL_BYTES    (STATS_SERIAL_PALABRAS * 8)

// Inicializa los contadores en cero y marca el tiempo de inicio.
stats_resultado_t stats_inicializar(void);

// Apaga el modulo. Despues se puede volver a inicializar.
void stats_liberar(void);

// Reinicia contadores y tiempo de inicio sin liberar el modulo.
stats_resultado_t stats_reiniciar(void);

// Suma 1 al contador del evento.
stats_resultado_t stats_registrar(stats_evento_t evento);

// Suma 'cantidad' al contador del evento (por ejemplo en el caso de los bytes).
stats_resultado_t stats_sumar(stats_evento_t evento, uint64_t cantidad);

// Registra cuanto duro resolver un fallo de pagina (SIGSEGV).
stats_resultado_t stats_registrar_latencia_fallo(uint64_t microsegundos);

// Copia atomica de todos los contadores.
stats_resultado_t stats_obtener(stats_snapshot_t *salida);

//se ordena el mensaje
stats_resultado_t stats_serializar(const stats_snapshot_t *snap, void *buffer, size_t capacidad);

// Operacion inversa: lee un snapshot recibido de otro nodo.
stats_resultado_t stats_deserializar(const void *buffer, size_t longitud,
                                     stats_snapshot_t *salida);

// Imprime un resumen legible. memoria_total y memoria_libre vienen de dsm_estado_t
void stats_imprimir(FILE *salida, int nodo_id, const stats_snapshot_t *snap, uint64_t memoria_total, uint64_t memoria_libre);

// para logs
const char *stats_nombre_evento(stats_evento_t evento);

#endif