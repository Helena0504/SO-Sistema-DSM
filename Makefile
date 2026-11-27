CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -pthread

ASIG = sistema/servicios/asignacion/asig.c
NODOS = sistema/servicios/nodos/nodos.c
PAGER = sistema/servicios/paginador/pager.c
PAGER_NODOS = sistema/servicios/paginador/pager_nodos.c
RED = sistema/red/red.c sistema/red/puente.c
STATS = sistema/servicios/estadisticas/stats.c
CONFIG = sistema/global/config.c
LOGS = sistema/global/logs/logs.c
API = sistema/api/api.c
MODULOS = $(ASIG) $(NODOS) $(PAGER) $(PAGER_NODOS) $(RED) $(STATS) $(CONFIG) $(LOGS)

PRUEBAS = test_global test_asig test_asig_memoria test_nodos \
          test_nodos_heartbeat test_pager test_pager_fallos \
          test_pager_nodos test_integracion test_api test_stats test_puente test_api_red test_api_fallos_red test_api_regiones_remotas test_api_reserva_distribuida test_api_msi_concurrencia test_api_msi_escalabilidad
BINARIOS = $(addprefix build/,$(PRUEBAS))

HTTP = sistema/app/cluster/http.c

.PHONY: all test test-integracion test-red test-msi-escala demo cluster clean

all: $(BINARIOS) build/prototipo_local

build:
	mkdir -p build

build/test_global: pruebas/test_global.c | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_asig: pruebas/test_asig.c $(ASIG) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_asig_memoria: pruebas/test_asig_memoria.c $(ASIG) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_nodos: pruebas/test_nodos.c $(NODOS) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_nodos_heartbeat: pruebas/test_nodos_heartbeat.c $(NODOS) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_pager: pruebas/test_pager.c $(ASIG) $(PAGER) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_pager_fallos: pruebas/test_pager_fallos.c $(ASIG) $(PAGER) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_pager_nodos: pruebas/test_pager_nodos.c $(ASIG) $(NODOS) $(PAGER) $(PAGER_NODOS) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_integracion: pruebas/test_integracion.c $(ASIG) $(NODOS) $(PAGER) $(PAGER_NODOS) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_api: pruebas/test_api.c $(API) $(MODULOS) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_stats: pruebas/test_stats.c $(STATS) | build
	$(CC) $(CFLAGS) -Isistema/servicios/estadisticas $^ -o $@

build/test_puente: pruebas/test_puente.c $(MODULOS) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_api_red: pruebas/test_api_red.c $(API) $(MODULOS) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_api_fallos_red: pruebas/test_api_fallos_red.c $(API) $(MODULOS) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_api_regiones_remotas: pruebas/test_api_regiones_remotas.c $(API) $(MODULOS) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_api_reserva_distribuida: pruebas/test_api_reserva_distribuida.c $(API) $(MODULOS) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_api_msi_concurrencia: pruebas/test_api_msi_concurrencia.c $(API) $(MODULOS) | build
	$(CC) $(CFLAGS) $^ -o $@

build/test_api_msi_escalabilidad: pruebas/test_api_msi_escalabilidad.c $(API) $(MODULOS) | build
	$(CC) $(CFLAGS) $^ -o $@

build/prototipo_local: sistema/app/demo/prototipo_local.c $(ASIG) $(NODOS) $(PAGER) $(PAGER_NODOS) | build
	$(CC) $(CFLAGS) $^ -o $@

test: $(BINARIOS)
	@set -e; for prueba in $(BINARIOS); do ./$$prueba; done

test-integracion: build/test_integracion
	./build/test_integracion

test-msi-escala: build/test_api_msi_escalabilidad
	DSM_MSI_NODOS=8 ./build/test_api_msi_escalabilidad
	DSM_MSI_NODOS=16 ./build/test_api_msi_escalabilidad
	DSM_MSI_NODOS=20 ./build/test_api_msi_escalabilidad

test-red: build/test_puente build/test_api_red build/test_api_fallos_red
	./build/test_puente
	./build/test_api_red
	./build/test_api_fallos_red

demo: build/prototipo_local
	./build/prototipo_local

clean:
	rm -rf build

# Ejecutable para correr el mismo programa en todas las computadoras.
build/dsm_nodo: sistema/app/cluster/main.c $(HTTP) $(API) $(MODULOS) | build
	$(CC) $(CFLAGS) $^ -o $@

cluster: build/dsm_nodo
	./build/dsm_nodo sistema/sistema.conf

# Reglas de la aplicacion Huffman
-include sistema/app/huffman/huffman.mk
	
	
	
# ---- app1: Image Filter Engine sobre DSM (C++ + OpenMP) ----
# Los modulos del DSM (C) se compilan con gcc y se enlazan con g++.
APP1_DIR = sistema/app/app1
APP1_OBJS = $(patsubst %.c,build/obj/%.o,$(API) $(MODULOS))
 
build/obj/%.o: %.c | build
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
 
build/app1: $(APP1_DIR)/src/main.cpp $(APP1_OBJS) | build
	g++ -std=c++17 -O2 -Wall -Wextra -pthread -fopenmp $^ -o $@
 
app1: build/app1
	./build/app1 sistema/sistema.conf

# Prueba de extremo a extremo: Huffman en dos nodos con lectura remota automatica.
.PHONY: test-huffman-cluster
test-huffman-cluster:
	python3 sistema/app/huffman/preparar_cluster.py
	$(MAKE) build/huffman_cluster
	python3 pruebas/test_huffman_cluster.py

-include sistema/app/huffman/huffman_cluster.mk

# Image Filter Engine: dos nodos y recuperacion remota automatica.
.PHONY: test-image-filter-cluster
test-image-filter-cluster: build/app1
	python3 pruebas/test_image_filter_cluster.py
