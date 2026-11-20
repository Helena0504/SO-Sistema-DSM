#ifndef HUFFMAN_DSM_BUFFERS_H
#define HUFFMAN_DSM_BUFFERS_H

#include <cstddef>
#include <memory>

// La API esta implementada en C; estas declaraciones conservan sus nombres al enlazar.
extern "C" {
#include "api.h"
}

class SesionDsm {
public:
    explicit SesionDsm(const char *config) : iniciada_(sistema_init(config) == OK) {}

    ~SesionDsm() {
        if (iniciada_) {
            (void)sistema_finalize();
        }
    }

    bool iniciada() const { return iniciada_; }
    SesionDsm(const SesionDsm &) = delete;
    SesionDsm &operator=(const SesionDsm &) = delete;

private:
    bool iniciada_;
};

struct LiberarDsm {
    void operator()(char *puntero) const noexcept {
        if (puntero != nullptr) {
            (void)sistema_free(puntero);
        }
    }
};

using BufferDsm = std::unique_ptr<char, LiberarDsm>;

inline BufferDsm reservarBufferDsm(std::size_t bytes) {
    return BufferDsm(static_cast<char *>(sistema_alloc(bytes)));
}

#endif
