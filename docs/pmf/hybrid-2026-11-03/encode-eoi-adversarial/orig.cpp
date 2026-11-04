#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <memory>
#include <vector>
// deps verbatim: dec/output_chunk.h OutputChunk; JPEGData reduced to tail_data (only field read);
// SerializationState reduced to output_queue (only field touched) -- STUBS, recorded.
struct OutputChunk {
  template <typename Bytes>
  OutputChunk(Bytes& bytes) : len(bytes.size()) {
    const void* src = bytes.data();
    next = reinterpret_cast<const uint8_t*>(src);
  }
  OutputChunk(const uint8_t* data, size_t size) : next(data), len(size) {}
  explicit OutputChunk(size_t size = 0) {
    buffer.reset(new std::vector<uint8_t>(size));
    next = buffer->data();
    len = size;
  }
  OutputChunk(std::initializer_list<uint8_t> bytes) {
    buffer.reset(new std::vector<uint8_t>(bytes));
    next = buffer->data();
    len = bytes.size();
  }
  const uint8_t* next;
  size_t len;
  std::unique_ptr<std::vector<uint8_t>> buffer;
};
struct JPEGData { std::vector<uint8_t> tail_data; };
struct SerializationState { std::deque<OutputChunk> output_queue; };
// verbatim: dec/jpeg_data_writer.cc:313-317
bool EncodeEOI(const JPEGData& jpg, SerializationState* state) {
  state->output_queue.push_back(OutputChunk({0xFF, 0xD9}));
  state->output_queue.emplace_back(jpg.tail_data);
  return true;
}
void RunAndPrint(const std::vector<uint8_t>& tail, int pre, bool mutate_after) {
  JPEGData jpg;
  jpg.tail_data = tail;
  SerializationState st;
  for (int k = 0; k < pre; ++k) st.output_queue.push_back(OutputChunk({static_cast<uint8_t>(k)}));
  bool ok = EncodeEOI(jpg, &st);
  if (mutate_after) { for (auto& b : jpg.tail_data) b = static_cast<uint8_t>(b + 1); }
  printf("ok%d chunks%zu:", (int)ok, st.output_queue.size());
  for (auto& c : st.output_queue) { printf(" ["); for (size_t i = 0; i < c.len; ++i) printf("%u,", c.next[i]); printf("]"); }
  printf("\n");
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
