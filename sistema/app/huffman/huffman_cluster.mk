# Un nodo DSM ejecuta la aplicación Huffman dentro de su mismo proceso.
# Se incluye después de las reglas del adaptador inicial.
build/huffman_cluster: sistema/app/huffman/huffman_cluster.cpp sistema/app/huffman/Source_cluster.cpp sistema/app/huffman/dsm_buffers.h $(HUFFMAN_C_OBJS)
	$(CXX) -std=c++11 -Wall -Wextra -pthread -Isistema/api -Isistema/global -Isistema/app/huffman sistema/app/huffman/huffman_cluster.cpp sistema/app/huffman/Source_cluster.cpp $(HUFFMAN_C_OBJS) -o $@

.PHONY: huffman-cluster
huffman-cluster: build/huffman_cluster
