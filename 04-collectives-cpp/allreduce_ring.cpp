// Day 4 implementation goes here.
//
// Plan:
//   void allreduce_ring(const RankInfo& info, float* data, size_t count, Op op) {
//       int N = info.world_size, r = info.rank;
//       size_t chunk = count / N;            // assumes divisible (TODO: remainder)
//       int next = (r + 1) % N, prev = (r - 1 + N) % N;
//
//       // Reduce-scatter: N-1 steps
//       std::vector<float> tmp(chunk);
//       for (int step = 0; step < N - 1; ++step) {
//           int send_chunk = (r - step + N) % N;
//           int recv_chunk = (r - step - 1 + N) % N;
//           // send-recv pair (use a separate thread or non-blocking send)
//           info.xport->send(next, data + send_chunk*chunk, chunk*sizeof(float));
//           info.xport->recv(prev, tmp.data(),              chunk*sizeof(float));
//           // accumulate
//           for (size_t i = 0; i < chunk; ++i)
//               data[recv_chunk*chunk + i] = reduce(op, data[recv_chunk*chunk + i], tmp[i]);
//       }
//       // All-gather: N-1 steps, identical pattern but no accumulation
//       for (int step = 0; step < N - 1; ++step) {
//           int send_chunk = (r - step + 1 + N) % N;
//           int recv_chunk = (r - step + N) % N;
//           info.xport->send(next, data + send_chunk*chunk, chunk*sizeof(float));
//           info.xport->recv(prev, data + recv_chunk*chunk, chunk*sizeof(float));
//       }
//   }

#include "collective.hpp"

namespace ip::collective {

void allreduce_ring(const RankInfo& info,
                    float* data,
                    std::size_t count,
                    Op op) {
    (void)info; (void)data; (void)count; (void)op;
    // TODO
}

}  // namespace ip::collective
