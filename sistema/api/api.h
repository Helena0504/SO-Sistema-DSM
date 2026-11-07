// ============================================================
// api.h
// Este documento se encarga de definir las estructuras, funciones y parametros del api
///////// Solo es un borrador inicial ///////////
// ============================================================

#ifndef API_H
#define API_H
#include <stddef.h>
#include <stdint.h>


// Deteccion de errores
typedef enum {
    OK = 0,
    ERROR_CONFIG,  // Error en la configuracion    
    ERROR_RED,     // Error en la red  
    ERROR_MEM,     // Error de memoria, se quedo sin memoria
    ERROR_NODE,    // Se desconecto un nodo 
    ERROR_NO_INIT,  // no ha sido inicializado   
    ERROR_ARG       // error de puntero invalido 
} status_t;

// Estadisticas preliminares
typedef struct {
    int faults_pagina; //cuantas veces tuvo que ir a buscar la pagina en otro nodo
    int paginas_enviadas; //cuantas paginas tuvo que enviar a otro nodo
    int paginas_recibidas; //cuantas paginas tuvo que traer de otro nodo
    int invalidadas;      //cuantas paginas fueron invalidadas
    size_t mem_total;      // memoria total del nodo local 
    size_t mem_usada;   // memoria usada del nodo local
} stats_t;


// INICIO
// inicializacion utilizando el archivo de configuracion 
status_t sistema_init(const char *config_path); //espera a que todos esten conectados y OK

void *sistema_alloc(size_t size); //reserva memoria que se necesita

status_t sistema_wait_ready(void); //espera a que todos tengan la memoria reservada

//EJECUCION
//----- Sincronizacion -------------
status_t sistema_block_all(void); //bloquea al sistema completo hasta que puedan seguir
status_t sistema_lock(int lock_id); //bloqueo mutex para escritura
status_t sistema_unlock(int lock_id); //desbloqueo mutex para escritura

//FIN
status_t sistema_free(void *ptr); //libera memoria
status_t sistema_finalize(void); // termina todo

// CONSULTAS
int sistema_node_id(void); //consulta id del nodo
int sistema_node_cantidad(void); //consulta numero de nodo
status_t sistema_stats(int node_id, stats_t *out);  //consulta de estadisticas
void sistema_print_nodes(void); //imprime la lista de nodos
void sistema_print_page_table(int node_id); //imprime la tabla de paginas
status_t sistema_last_error(void); //imprime el ultimo error


#endif