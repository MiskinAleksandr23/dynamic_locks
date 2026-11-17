#include "locktree_port.hpp"

#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace locktree_port {
namespace {
using ROCKSDB_NAMESPACE::Status;
using ROCKSDB_NAMESPACE::TransactionDBCondVar;
using ROCKSDB_NAMESPACE::TransactionDBMutex;
using ROCKSDB_NAMESPACE::TransactionDBMutexFactory;

void Check(int result, const char *operation) {
  if (result != 0) throw std::system_error(result, std::generic_category(), operation);
}

class PosixMutex final : public TransactionDBMutex {
 public:
  PosixMutex() {
    pthread_mutexattr_t attr;
    Check(pthread_mutexattr_init(&attr), "pthread_mutexattr_init");
    const int type_result = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
    const int init_result = type_result == 0 ? pthread_mutex_init(&mutex_, &attr) : type_result;
    const int destroy_result = pthread_mutexattr_destroy(&attr);
    Check(init_result, "pthread_mutex_init/normal type");
    if (destroy_result != 0) {
      pthread_mutex_destroy(&mutex_);
      Check(destroy_result, "pthread_mutexattr_destroy");
    }
  }
  ~PosixMutex() override {
    if (pthread_mutex_destroy(&mutex_) != 0) std::terminate();
  }
  PosixMutex(const PosixMutex &) = delete;
  PosixMutex &operator=(const PosixMutex &) = delete;

  Status Lock() override {
    Check(pthread_mutex_lock(&mutex_), "pthread_mutex_lock");
    return Status::OK();
  }
  void UnLock() override {
    Check(pthread_mutex_unlock(&mutex_), "pthread_mutex_unlock");
  }
  pthread_mutex_t *native_handle() noexcept { return &mutex_; }

 private:
  pthread_mutex_t mutex_;
};

class PosixCondVar final : public TransactionDBCondVar {
 public:
  PosixCondVar() { Check(pthread_cond_init(&condition_, nullptr), "pthread_cond_init"); }
  ~PosixCondVar() override {
    if (pthread_cond_destroy(&condition_) != 0) std::terminate();
  }
  PosixCondVar(const PosixCondVar &) = delete;
  PosixCondVar &operator=(const PosixCondVar &) = delete;

  Status WaitFor(std::shared_ptr<TransactionDBMutex> mutex, int64_t microseconds) override {
    auto *native = dynamic_cast<PosixMutex *>(mutex.get());
    if (native == nullptr) throw std::invalid_argument("LockTree condition and mutex factories differ");
    int result;
    if (microseconds < 0) {
      result = pthread_cond_wait(&condition_, native->native_handle());
    } else {
      // Upstream supplies a relative timeout. Default POSIX condition variables
      // use CLOCK_REALTIME on both macOS and Linux; convert once per wait.
      timespec deadline;
      if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
        throw std::system_error(errno, std::generic_category(), "clock_gettime");
      const int64_t seconds = microseconds / 1'000'000;
      const long nanoseconds = static_cast<long>(microseconds % 1'000'000) * 1000;
      const bool carry = deadline.tv_nsec + nanoseconds >= 1'000'000'000;
      if (seconds > std::numeric_limits<time_t>::max() - deadline.tv_sec - int(carry))
        throw std::overflow_error("LockTree timeout exceeds time_t");
      deadline.tv_sec += static_cast<time_t>(seconds) + int(carry);
      deadline.tv_nsec = (deadline.tv_nsec + nanoseconds) % 1'000'000'000;
      result = pthread_cond_timedwait(&condition_, native->native_handle(), &deadline);
    }
    if (result == ETIMEDOUT) return Status::TimedOut();
    Check(result, "pthread_cond_wait");
    return Status::OK(); // Upstream rechecks its predicate after spurious wakes.
  }
  void Notify() override { Check(pthread_cond_signal(&condition_), "pthread_cond_signal"); }
  void NotifyAll() override { Check(pthread_cond_broadcast(&condition_), "pthread_cond_broadcast"); }

 private:
  pthread_cond_t condition_;
};

class PosixFactory final : public TransactionDBMutexFactory {
 public:
  std::shared_ptr<TransactionDBMutex> AllocateMutex() override {
    return std::make_shared<PosixMutex>();
  }
  std::shared_ptr<TransactionDBCondVar> AllocateCondVar() override {
    return std::make_shared<PosixCondVar>();
  }
};
} // namespace

toku_external_mutex_factory_t MakeMutexFactory() {
  return std::make_shared<PosixFactory>();
}
} // namespace locktree_port
