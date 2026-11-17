#include "dynamic/paper_lock.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

int main() {
  constexpr size_t size = 1 << 16;
  constexpr size_t threads = 4;
  std::vector<size_t> data(size, 0);
  PaperRangeLock<64> lock(size);
  lock.StartRebuilder();
  std::vector<std::thread> workers;
  for (size_t worker = 0; worker < threads; ++worker) {
    workers.emplace_back([&] {
      for (size_t index = 0; index < size; ++index) {
        // Both endpoints are inclusive; a point is [index,index].
        lock.WriteQuery(index, index, [&](size_t left, size_t right) {
          for (size_t point = left; point <= right; ++point) {
            ++data[point];
          }
        });
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
  lock.StopRebuilder();
  lock.RethrowBackgroundError();
  if (!std::ranges::all_of(data, [](size_t value) { return value == threads; })) {
    throw std::runtime_error("incorrect array");
  }
  std::cout << "All elements verified\n";
}
