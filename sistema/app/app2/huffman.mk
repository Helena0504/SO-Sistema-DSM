# Aplicacion Huffman con buffers reservados mediante la API del DSM.
HUFFMAN_C_OBJS = $(patsubst %.c,build/huffman_obj/%.o,$(API) $(MODULOS))

build/huffman_obj/%.o: %.c | build
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/huffman_dsm: sistema/app/huffman/Source_dsm.cpp sistema/app/huffman/dsm_buffers.h $(HUFFMAN_C_OBJS)
	$(CXX) -std=c++11 -Wall -Wextra -pthread -Isistema/api -Isistema/app/huffman $< $(HUFFMAN_C_OBJS) -o $@

build/test_huffman_dsm_buffers: pruebas/test_huffman_dsm_buffers.cpp sistema/app/huffman/dsm_buffers.h $(HUFFMAN_C_OBJS)
	$(CXX) -std=c++11 -Wall -Wextra -pthread -Isistema/api -Isistema/app/huffman $< $(HUFFMAN_C_OBJS) -o $@

.PHONY: huffman-dsm test-huffman-dsm
huffman-dsm: build/huffman_dsm

test-huffman-dsm: build/test_huffman_dsm_buffers
	@sed -E 's@^nodos=.*@nodos=0:127.0.0.1@' sistema/sistema.conf > build/huffman_local.conf
	./build/test_huffman_dsm_buffers build/huffman_local.conf
