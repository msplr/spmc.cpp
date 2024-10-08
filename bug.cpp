#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

std::atomic<bool> stop{false};

void signalCallback(int) { stop = true; }

void busyWait(std::chrono::microseconds us) {
  auto start = std::chrono::high_resolution_clock::now();
  while (std::chrono::high_resolution_clock::now() - start < us) {
    // busy wait to hog the CPU
  }
}

void configThread(const char* name, int priority, int schedAffinity = -1) {
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

struct Msg {
  std::string value;
};

Msg message;
int64_t messageCounter = 0;
std::mutex message_mutex;
std::condition_variable message_cv;

void publish(const Msg& msg) {
  // Bug: even without locking, this can block the calling thread
  // std::lock_guard<std::mutex> lock(message_mutex);
  // message = msg;
  messageCounter++;
  message_cv.notify_all();
}

std::optional<Msg> receive(std::chrono::milliseconds timeout) {
  static int64_t lastMsg = -1;
  std::unique_lock<std::mutex> lock(message_mutex);
  if (message_cv.wait_for(lock, timeout, [] { return messageCounter > lastMsg; })) {
    lastMsg = messageCounter;
    return message;
  }
  return std::nullopt;
}

void producer(int prio = 42, int cpu = 0) {
  configThread("producer", prio, cpu);
  Msg msg;
  msg.value = "hello";
  while (!stop) {
    auto start = std::chrono::high_resolution_clock::now();
    publish(msg);
    auto end = std::chrono::high_resolution_clock::now();

    if (end - start > 1ms) {
      printf("Publish took %ld us!\n", std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    }

    std::this_thread::sleep_for(1ms);
  }
}

void consumer(int prio = -1, int cpu = -1) {
  configThread("consumer", prio, cpu);
  while (!stop) {
    auto maybeMsg = receive(1s);
    if (!maybeMsg) {
      printf("receive timeout!\n");
    }
  }
}

void cpuIntensive(int prio, int cpu) {
  configThread("cpuIntensive", prio, cpu);
  while (!stop) {
    busyWait(10ms);
    std::this_thread::sleep_for(10ms);
  }
}

void interrupt(int cpu) {
  configThread("interrupt", 50, cpu);
  printf("Interrupt %d\n", cpu);
  while (!stop) {
    std::this_thread::sleep_for(100us);
    busyWait(1us);
  }
}

// cpu pinning case, fixed priorities
void test0() {
  std::vector<std::thread> threads;
  threads.emplace_back(producer, /*prio=*/42, /*cpu=*/0);
  threads.emplace_back(consumer, /*prio=*/20, /*cpu=*/0);  // high prio consumer
  threads.emplace_back(consumer, /*prio=*/1, /*cpu=*/1);   // low prio consumer, can miss update
  threads.emplace_back(cpuIntensive, /*prio=*/2, /*cpu=*/1);
  threads.emplace_back(interrupt, /*cpu=*/0);  // regularly interrupt producer (makes race condition more likely)
  while (!stop) {
    std::this_thread::sleep_for(200ms);
    printf("%ld messages\n", messageCounter);
  }

  for (auto& t : threads) {
    t.join();
  }
}

// cpu pinning case
void test1() {
  std::vector<std::thread> threads;
  threads.emplace_back(producer, 42, 0);
  threads.emplace_back(consumer, -1, 0);  // fast consumer
  threads.emplace_back(consumer, -1, 1);  // slow consumer because of high CPU load
  threads.emplace_back(cpuIntensive, -1, 1);
  threads.emplace_back(interrupt, 0);  // regularly interrupt producer (makes race condition more likely)
  while (!stop) {
    std::this_thread::sleep_for(200ms);
    printf("%ld messages\n", messageCounter);
  }

  for (auto& t : threads) {
    t.join();
  }
}

// saturated system case
void test2() {
  int numCpu = 4;
  std::vector<std::thread> threads;
  threads.emplace_back(producer, 42, -1);  // high prio producer
  threads.emplace_back(consumer, 41, -1);  // high prio consumer
  threads.emplace_back(consumer, 1, -1);   // low prio consumer
  for (int cpu = 0; cpu < numCpu; cpu++) {
    threads.emplace_back(interrupt, cpu);  // make sure threads are interrupted on every cpu
    threads.emplace_back(cpuIntensive, 10, cpu);
  }
  while (!stop) {
    std::this_thread::sleep_for(200ms);
    printf("%ld messages\n", messageCounter);
  }

  for (auto& t : threads) {
    t.join();
  }
}

int main(int argc, char* argv[]) {
  signal(SIGINT, signalCallback);

  int testCase = 0;
  if (argc > 1) {
    testCase = std::atoi(argv[1]);
  }
  switch (testCase) {
    case 0:
      test0();
      break;
    case 1:
      test1();
      break;
    case 2:
      test2();
      break;
    default:
      break;
  }
  return 0;
}
