#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <climits>
#include <csignal>
#include <cstring>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static constexpr size_t BUF_SIZE = 3;

void configThread(const char *name, int priority, int schedAffinity = -1) {
  printf("Setting thread name to %s\n", name);
  pthread_setname_np(pthread_self(), name);

  if (priority != -1) {
    sched_param params{};
    params.sched_priority = priority;

    if (sched_setscheduler(0, SCHED_FIFO, &params) != 0) {
      printf("Failed to set process priority: %s. Check /etc/security/limits.conf for the rights.\n", strerror(errno));
      exit(-1);
    }
  }

  if (schedAffinity != -1) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(schedAffinity, &cpuset);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
      printf("Failed to set thread affinity: %s\n", strerror(errno));
      exit(-1);
    }
  }
}

template <typename T>
struct ShmTopic {
  T buffer[BUF_SIZE];
  std::atomic<T *> read_ptr{nullptr};
  char name[100];
};

template <typename T>
struct ShmPublisher {
  ShmTopic<T> &topic_;
  int write_index = 0;

  ShmPublisher(ShmTopic<T> &topic) : topic_(topic) {}

  bool publish(const T &msg) {
    getNextMessage() = msg;
    return publish();
  }

  bool publish() {
    // TODO: check memory ordering
    topic_.read_ptr.store(reinterpret_cast<T *>(&getNextMessage()), std::memory_order_release);

    // updated the the read pointer, wake up all waiting threads
    int res = syscall(SYS_futex, &topic_.read_ptr, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);

    // wrap around
    write_index = (write_index + 1) % BUF_SIZE;

    if (res == -1) {
      fprintf(stderr, "Error in futex wake\n");
      return false;
    }
    return true;
  }

  T &getNextMessage() { return topic_.buffer[write_index]; }
};

template <typename T>
struct ShmSubscriber {
  ShmTopic<T> &topic_;
  T *last_ptr = nullptr;
  std::atomic<bool> stop{false};

  ShmSubscriber(ShmTopic<T> &topic) : topic_(topic) {}

  void cancelReceive() {
    stop.store(true);

    // Wakes up all waiting threads but that's fine for shutdown case
    int res = syscall(SYS_futex, &topic_.read_ptr, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
    if (res == -1) {
      fprintf(stderr, "Error in futex wake\n");
    }
  }

  std::optional<T> receive(const std::chrono::nanoseconds &timeout) {
    // Wait until the shared pointer is updated or timeout occurs
    while (stop.load() == false) {
      // TODO: check memory order
      // Try to load the value of the shared pointer
      T *local_ptr = topic_.read_ptr.load(std::memory_order_acquire);

      if (local_ptr != nullptr && local_ptr != last_ptr) {
        last_ptr = local_ptr;
        T *value = reinterpret_cast<T *>(local_ptr);
        return *value;
      }

      // Wait on the futex (the pointer) with a timeout until it changes
      auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
      auto nanosecs = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - secs);

      struct timespec ts;
      ts.tv_sec = secs.count();
      ts.tv_nsec = nanosecs.count();

      int ret = syscall(SYS_futex, &topic_.read_ptr, FUTEX_WAIT, local_ptr, &ts, nullptr, 0);

      if (ret == -1 && errno == ETIMEDOUT) {
        return std::nullopt;
      } else if (ret == -1 && errno != EAGAIN) {
        fprintf(stderr, "Error in futex wait: %s\n", strerror(errno));
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  static timespec chronoToTimespec(const std::chrono::nanoseconds &chrono) {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(chrono);
    auto nanosecs = std::chrono::duration_cast<std::chrono::nanoseconds>(chrono - secs);

    struct timespec ts;
    ts.tv_sec = secs.count();
    ts.tv_nsec = nanosecs.count();
    return ts;
  }
};

struct Msg {
  int value;
  std::chrono::high_resolution_clock::time_point timestamp;
};
ShmTopic<Msg> msgTopic;
std::atomic<bool> stop{false};

void producer(int cpu) {
  configThread("producer", 1, cpu);
  ShmPublisher<Msg> pub(msgTopic);
  int i = 0;
  while (!stop) {
    Msg &msg = pub.getNextMessage();
    msg.value = i++;
    msg.timestamp = std::chrono::high_resolution_clock::now();
    auto start = std::chrono::high_resolution_clock::now();
    pub.publish();
    auto end = std::chrono::high_resolution_clock::now();
    double us = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0;
    printf("producer: publish message %d   publish time: %fus\n", msg.value, us);
    std::this_thread::sleep_for(100us);
  }
}

void consumer(int cpu) {
  configThread("consumer", -1, cpu);
  static std::atomic<int> thread_id_counter = 0;
  int thread_id = thread_id_counter++;
  ShmSubscriber<Msg> sub(msgTopic);
  int lastValue = -1;
  while (!stop) {
    auto maybeMsg = sub.receive(100ms);
    if (maybeMsg) {
      auto &msg = maybeMsg.value();
      auto now = std::chrono::high_resolution_clock::now();
      auto micros = std::chrono::duration_cast<std::chrono::nanoseconds>(now - msg.timestamp).count() * 1.0e-3;
      printf("consumer %d: got message %d  latency %fus\n", thread_id, msg.value, micros);
      if (msg.value != lastValue + 1 && lastValue != -1) {
        printf("consumer %d: missed %d messages\n", thread_id, msg.value - lastValue - 1);
      }
      lastValue = msg.value;
    } else {
      printf("consumer %d: timeout!\n", thread_id);
    }
  }
}

void busyWait(std::chrono::microseconds us) {
  auto start = std::chrono::high_resolution_clock::now();
  while (std::chrono::high_resolution_clock::now() - start < us) {
    // busy wait to hog the CPU
  }
}

void cpuIntensive(int prio, int cpu) {
  configThread("cpuIntensive", prio, cpu);
  while (!stop) {
    busyWait(10ms);
    std::this_thread::sleep_for(10ms);
  }
}

void signalCallback(int) { stop.store(true); }

int main() {
  signal(SIGINT, signalCallback);

  std::vector<std::thread> threads;
  threads.emplace_back(consumer, 0);
  threads.emplace_back(consumer, 1);
  threads.emplace_back(consumer, -1);
  threads.emplace_back(consumer, -1);
  threads.emplace_back(cpuIntensive, 1, 1);
  threads.emplace_back(producer, 0);

  while (!stop) {
    std::this_thread::sleep_for(100ms);
  }

  for (auto &t : threads) {
    t.join();
  }
  return 0;
}
