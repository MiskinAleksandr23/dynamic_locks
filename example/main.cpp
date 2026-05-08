#include "dynamic/dynamic_lock.hpp"

static constexpr int kLockSize = 1 << 20;
static constexpr int kLocks = 64;
static constexpr int kThreadCnt = 8;

int main() {
    std::vector<std::thread> ThreadPoll;

    DynamicLock<kLocks> dynamic_lock(kLockSize);

    ThreadPoll.reserve(kThreadCnt);
    std::vector data(kLockSize, 0);

    auto AddOneToPoint = [&data](int left, int) {
        ++data[left];
    };

    for (int idx = 0; idx < kThreadCnt; ++idx) {
        ThreadPoll.emplace_back([&dynamic_lock, &AddOneToPoint] {
            for (int i = 0; i < kLockSize; ++i) {
                dynamic_lock.WriteQuery(i, i + 1, AddOneToPoint);
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
