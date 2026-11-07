// ============================================================
// api.c
// Implementa el api que coordina los servicios
///////ESTO SOLO ES UN BORRADOR ///////
///////FALTA LA IMPLEMENTACION DE LOS SERVICIOS ////////
// ============================================================

#include <pthread.h>
#include <stdint.h>
#include "api.h"
#include "../servicios/asignacion/asig.h" 
#include "../servicios/estadisticas/stats.h" 
#include "../servicios/nodos/nodos.h" 
#include "../servicios/paginador/pager.h" 

static int initialized = 0; //cuando el sistema este inicializado, cambia a 1

static pthread_key_t  error_key; //Hilo de errores
static pthread_once_t error_once = PTHREAD_ONCE_INIT; //es una key distinta por cada hilo

//cuando ocurre un error crea un hilo
static void error_key_create(void) {
    pthread_key_create(&error_key, NULL);
}

//asigna el valor del error
static void set_error(status_t e) {
    pthread_once(&error_once, error_key_create);
    pthread_setspecific(error_key, (void *)(intptr_t)e);
}

// en caso de error se crea el hilo de error. Es una macro
#define FALLO(e) do { \
    set_error(e);     \
    return (e);       \
} while (0)


// se queda en espera hasta que este inicializado
#define REQUIERE_INIT() do {     \
    if (!initialized)            \
    {                            \
        FALLO(ERROR_NO_INIT);    \
    }                            \
} while (0)

//Inicializacion
status_t sistema_init(const char *config_path)
{
    if (!config_path) //el config path no existe
    {
        FALLO(ERROR_ARG);
    }
    if (initialized) // ya estaba iniciado
    {
        return OK;   
    }

    status_t estado = 0////AGREGAR INICIALIZACION DE SERVICIOS
    if (estado != OK) //Si no esta OK, deshacer
    {
        //AGREGAR FUNCION DE SERVICIOS: MATAR TODO   
        FALLO(estado);
    }
    initialized = 1;
    return OK;
}

// Reservar memoria
void *sistema_alloc(size_t size)
{
    if (!initialized) //no estaba inicializado
    {
        set_error(ERROR_NO_INIT);
        return NULL;
    }
    if (size == 0) //cantidad de memoria no valida
    {
        set_error(ERROR_ARG);
        return NULL;
    }

    void *puntero = 0 //AGREGAR FUNCION DE ASIG.H: ASIGNACION DE PAGINAS
    if (!puntero) //Si no devuelve ninguna direccion, da error
    {
        set_error(ERROR_MEM);
    }
    return puntero;
}

// Espera a que todos los nodos esten listos y tengan memoria reservada
status_t sistema_wait_ready(void)
{
    REQUIERE_INIT(); //espera
    status_t estado = 0///AGREGAR FUNCION DE NODOS.H: ESPERA A TODOS
    if (estado != OK)
    {
        FALLO(estado);
    }
    return OK;
}

// SIncronizacion de lectura y escritura
status_t sistema_block_all(void) //bloquea a todos de ser necesario
{
    REQUIERE_INIT();
    status_t estado = 0///AGREGRA FUNCION DE NODOS.H: BLOQUEAR TODOS
    if (estado != OK)
    {
        FALLO(estado);
    }
    return OK;
}

//Mutex
status_t sistema_lock(int lock_id)
{
    REQUIERE_INIT();
    if (lock_id < 0)
    {
        FALLO(ERROR_ARG);
    }
    status_t estado = 0////AGREGAR FUNCION DE NODOS: LOCK
    if (estado != OK)
    {
        FALLO(estado);
    }
    return OK;
}

status_t sistema_unlock(int lock_id)
{
    REQUIERE_INIT();
    if (lock_id < 0)
    {
        FALLO(ERROR_ARG);
    }
    status_t estado = 0///AGREGAR FUNCION DE NODOS: UNLOCK
    if (estado != OK)
    {
        FALLO(estado);
    }
    return OK;
}

// FIN
status_t sistema_free(void *puntero) //libera todo
{
    REQUIERE_INIT();
    if (!puntero)
    {
        FALLO(ERROR_ARG);
    }
    status_t estado = 0///LLAMAR A FUNCION DE ASIGNACION: LIBERAR
    if (estado != OK)
    {
        FALLO(estado);
    }
    return OK;
}

status_t sistema_finalize(void)
{
    REQUIERE_INIT();
    ///AGREGRA FUNCION DE NODOS.h: BLOQUEAR TODOS
    //AGREGAR FUNCION DE SERVICIOS: MATAR TODO 
    initialized = 0;
    return OK;
}

// CONSULTAS
int sistema_node_id(void)
{
    if (!initialized)
    {
        set_error(ERROR_NO_INIT);
        return -1;
    }
    return 0///FUNCION DE NODOS.h DEVOLVER ID ACTUAL
}

int sistema_node_cantidad(void)
{
    if (!initialized)
    {
        set_error(ERROR_NO_INIT);
        return -1;
    }
    return 0///FUNCION DE NODOS.h DEVOLVER VIVOS
}

status_t sistema_stats(int node_id, stats_t *out)
{
    REQUIERE_INIT();
    if (!out)
    {
        FALLO(ERROR_ARG);
    }
    status_t estado = 0///FUNCION DE STATS.h DEVOLVER STATS
    if (estado != OK)
    {
        FALLO(estado);
    }
    return OK;
}

void sistema_print_nodes(void)
{
    if (!initialized)
    {
        set_error(ERROR_NO_INIT);
        return;
    }
    ;///FUNCION DE NODOS.h IMPRIMIR LISTA DE NODOS
}

void sistema_print_page_table(int node_id)
{
    if (!initialized)
    {
        set_error(ERROR_NO_INIT);
        return;
    }
   ;///FUNCION DE PAGER.h IMPRIMIR TABLA DE PAGINAS
}

// Devuelve el ultimo error del hilo que llama
status_t sistema_last_error(void)
{
    pthread_once(&error_once, error_key_create);
    return (status_t)(intptr_t)pthread_getspecific(error_key);
}