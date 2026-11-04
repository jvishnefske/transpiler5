#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <vector>

struct Chunk {
    std::vector<uint8_t> bytes;
};

bool EncodeEOI(const std::vector<uint8_t>& tail_data, std::vector<Chunk>* output_queue) {
    const std::vector<uint8_t> eoi = {static_cast<uint8_t>(0xFF), static_cast<uint8_t>(0xD9)};
    output_queue->push_back(Chunk{eoi});
    output_queue->push_back(Chunk{tail_data});
    return true;
}

void RunAndPrint(const std::vector<uint8_t>& tail, int pre, bool mutate_after) {
    std::vector<uint8_t> tail_data = tail;
    std::vector<Chunk> q;
    for (int k = 0; k < pre; ++k) {
        const std::vector<uint8_t> one = {static_cast<uint8_t>(k)};
        q.push_back(Chunk{one});
    }
    const bool ok = EncodeEOI(tail_data, &q);
    if (mutate_after) {
        for (std::size_t i = 0; i < tail_data.size(); ++i) {
            tail_data[i] = static_cast<uint8_t>(tail_data[i] + 1);
        }
    }
    std::printf("ok%d chunks%zu:", static_cast<int>(ok), q.size());
    for (std::size_t c = 0; c < q.size(); ++c) {
        std::printf(" [");
        for (std::size_t i = 0; i < q[c].bytes.size(); ++i) {
            std::printf("%u,", static_cast<unsigned>(q[c].bytes[i]));
        }
        std::printf("]");
    }
    std::printf("\n");
}

uint32_t NextRand(uint32_t* s) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

int main() {
    uint32_t seed = 1616u;
    for (int trial = 0; trial < 50; ++trial) {
        const std::size_t n = static_cast<std::size_t>((NextRand(&seed) >> 16) % 9u);
        std::vector<uint8_t> tail(n, static_cast<uint8_t>(0));
        for (std::size_t i = 0; i < n; ++i) {
            tail[i] = static_cast<uint8_t>(NextRand(&seed) >> 24);
        }
        RunAndPrint(tail, trial % 3, true);
    }
    return 0;
}
