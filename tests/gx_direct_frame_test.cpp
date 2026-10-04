#include "moderngekko/gx_direct_frame.hpp"
#include <thread>

int main()
{
  using namespace moderngekko;
  GxDirectFrameQueue queue;
  GxLiveFrame input, output;
  input.states.resize(1); input.draws.resize(1); input.draws[0].vertices.resize(64);
  const auto* state_pointer = input.states.data();
  const auto* vertex_pointer = input.draws[0].vertices.data();
  input.frame = 123;
  if (!queue.Publish(input) || !queue.Take(output)) return 1;
  if (output.frame != 123 || output.states.data() != state_pointer || output.draws[0].vertices.data() != vertex_pointer) return 2;
  if (queue.Take(input)) return 3;
  queue.RequestFrame();
  if (!queue.Requested() || queue.Requested()) return 4;
  std::thread producer([&] {
    for (unsigned i = 0; i < 100; ++i)
    {
      input.frame = i; input.draws.resize(1); input.draws[0].vertices.resize(64);
      input.draws[0].vertices[0].color[0] = i;
      if (!queue.Publish(input)) break;
    }
  });
  bool valid = true;
  for (unsigned i = 0; i < 100; ++i)
  {
    while (!queue.Take(output)) std::this_thread::yield();
    valid &= output.frame == i && output.draws[0].vertices[0].color[0] == i;
  }
  producer.join();
  if (!valid) return 5;
  // Stop must release a producer blocked behind an unconsumed frame.
  if (!queue.Publish(input)) return 6;
  bool published = true;
  std::thread blocked([&] { published = queue.Publish(input); });
  queue.Stop(); blocked.join();
  if (published || !queue.Stopped() || queue.Take(output)) return 7;
  return 0;
}
