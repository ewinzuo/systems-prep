// Day 5: binomial-tree broadcast. Stub.
//
// Plan:
//   void broadcast_tree(const RankInfo& info, void* buf, size_t len, int root) {
//       // shift ranks so root is 0
//       int rel = (info.rank - root + info.world_size) % info.world_size;
//       int N = info.world_size;
//       for (int d = 1; d < N; d <<= 1) {
//           if (rel < d) {
//               // I have data; send to rel + d if exists
//               int peer_rel = rel + d;
//               if (peer_rel < N) {
//                   int peer = (peer_rel + root) % N;
//                   info.xport->send(peer, buf, len);
//               }
//           } else if (rel < (d << 1)) {
//               int peer_rel = rel - d;
//               int peer = (peer_rel + root) % N;
//               info.xport->recv(peer, buf, len);
//           }
//       }
//   }

#include "collective.hpp"

namespace ip::collective {

void broadcast_tree(const RankInfo& info, void* buf, std::size_t len, int root) {
    (void)info; (void)buf; (void)len; (void)root;
    // TODO
}

}  // namespace ip::collective
