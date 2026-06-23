# Dynamic Locks

Пример из [`example/main.cpp`](example/main.cpp):

```cpp
#include "dynamic/dynamic_lock.hpp"

static constexpr int kLockSize = 1 << 20;
static constexpr int kLocks = 64;
static constexpr int kThreadCnt = 8;

int main() {
    std::vector<std::thread> ThreadPoll;
    ThreadPoll.reserve(kThreadCnt);

    std::vector data(kLockSize, 0);
    
    // Пример запроса: прибавление 1 на отрезке
    auto AddOneToPoint = [&data](int left, int right) {
        for (int idx = left; idx < right; ++idx) {
            ++data[idx];
        }
    };

    // Не владаеет данными, нужен только размер "охраняемых" данных
    DynamicLock<kLocks> lock(kLockSize);

    for (int idx = 0; idx < kThreadCnt; ++idx) {
        ThreadPoll.emplace_back([&lock, &AddOneToPoint] {
            // Каждый поток будет делать прибавления в точках последовательно
            for (int i = 0; i < kLockSize; ++i) {
                lock.WriteQuery(i, i + 1, AddOneToPoint);
            }
        });
    }
    for (auto &&th: ThreadPoll) {
        th.join();
    }

    if (!std::ranges::all_of(data, [](int value) { return value == kThreadCnt; })) {
        throw std::runtime_error("Incorrect result");
    }
}

```
