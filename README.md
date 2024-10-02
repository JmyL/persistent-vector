# Task

In `task.cc` you will find the interface of a sample class `vector` as well as
a set of unit tests. Your task is to implement the functions defined in `vector`
so that they comply with the following requirements:

- the contents of `vector` are persisted to the directory given to the c'tor.
- the `vector` component recreates a previous state from the persistence directory
  when created
- the persistence schema must account for unexpected service shutdowns, like in the
  case of power outages
- the functions produce the same results as their STL counterparts, ie.
  `push_back` will append a string to the end of the `vector`
- all test cases shall pass

You may assume that:
- your data directory has unlimited disk space
- strings added to the vector are less than 4K long
- you can ignore RAM limitations


# Implementation

## Assumptions

1. 8 exabytes of storage and memory support is sufficient.
1. The filesystem used supports metadata journaling and files up to 8 exabytes (e.g. XFS).
1. Size of vector doesn't cross 2^64.
1. Accept zero length string as a valid item.
 

## Current design (not proud of)

### Binary File

Store data as a binary log file with the format below.

`push_back` command:

| **Command** | **Id** | **DSize** | **Data**        |
|-------------|--------|-----------|-----------------|
| 1 (8 bits)  | 8 bits | 8 bits    | DSize bits long |

### `erase` command:

| **Command** | **RId** | **RIndex** |
|-------------|---------|------------|
| 2 (8 bits)  | 8 bits  | 8 bits     |

2. Id: Unique identifier for `push_back` command, used for debugging. This may be rotated.
3. DSize: Byte size of the data field.
4. Data: Data for a push command. For an erase command, it indicates the index to be removed.
4. RId: Id of command to be removed.
4. RIndex: Index to be removed.
4. Pad: Automatically added to align the next packet to an 8-byte boundary.

### Calling `fsync`

We can rely on the background process pdflush, but it flushes every modified
kernel buffer at a fixed time interval of 30 seconds.

I added a background thread to manage a backup file by myself.

----

# New Design using `io_uring`, `DIRECT_IO` and `coroutines`

With `io_uring` and `DIRECT_IO`, file I/O could be as fast as on a bare-metal machine. Furthermore, repeatedly calling fsync is not a proper solution for fsync failure. We should be aware of the packet we wrote and repeat it, or write it somewhere else if the problem is physical issue.

## Experiment: using `io_uring`

I stacked `x` requests in a row and wrote it using `io_uring` with a size of y for the submission queue `y`.
My design has patch for some number of packets and `io_uring` for number of batches.
I adjusted these two variables to handle one million API calls with the code below.
I handled all requests in a single thread.


### Benchmark Code

```cpp
#include <benchmark/benchmark.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>

using namespace std::chrono_literals;

#define DIV_UP(x, y) (((x) + (y) - 1) / (y))

constexpr unsigned long long operator"" _GB(unsigned long long num) {
  return num * 1024 * 1024 * 1024;
}
constexpr long long operator"" _KB(unsigned long long num) {
  return num * 1024;
}

#define SECTOR_SIZE 4096

static void BM_uring(benchmark::State &state) {
  auto i = 0u;
  constexpr auto filename = ".tmp.bin";
  struct io_uring m_ring;

  if (std::filesystem::exists(filename)) {
    if (std::filesystem::remove(filename)) {
    } else {
      std::cout << "File not found.\n";
      exit(0);
    }
  }
  // auto fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 644);
  // pthread_t thread;
  int fd;
  char *buf;
  struct io_uring_sqe *sqe;
  int ret;

  fd = open("output.bin", O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
  if (fd < 0) {
    perror("open");
    return;
  }
  __off_t offset = 0;

  auto NUM_OF_ITEMS = 131_KB;  // near 1 million bytes
  auto ITEM_SIZE = 4_KB * 2;
  auto SENTENCE_SIZE = state.range(0);
  auto PARAGRAPH_SIZE = state.range(1);
  auto NUM_OF_ITERATION =
      DIV_UP(NUM_OF_ITEMS, (SENTENCE_SIZE * PARAGRAPH_SIZE));
  auto BATCH_SIZE = ITEM_SIZE * SENTENCE_SIZE;

  io_uring_queue_init(PARAGRAPH_SIZE * 2, &m_ring, 0);

  ret = posix_memalign((void **)&buf, SECTOR_SIZE, BATCH_SIZE);
  if (ret != 0) {
    std::cerr << "posix_memalign failed: " << strerror(ret) << std::endl;
    return;
  }

  std::mutex mtx;
  std::condition_variable cv;

  auto size = BATCH_SIZE;

  for (auto _ : state) {
    for (auto i = 0u; i < NUM_OF_ITERATION; ++i) {
      memset(buf, 'A' + i, BATCH_SIZE);
      // std::cout << i << std::endl;

      for (auto i = 0u; i < PARAGRAPH_SIZE; ++i) {
        struct io_uring_sqe *sqe;
        struct io_uring_cqe *cqe;
        sqe = io_uring_get_sqe(&m_ring);
        io_uring_prep_write(sqe, fd, buf, size, offset);
        sqe->user_data = 1;
        sqe->flags |= IOSQE_IO_HARDLINK_BIT;
        offset += size;
      }

      // fsync
      {
        struct io_uring_sqe *sqe;
        struct io_uring_cqe *cqe;
        sqe = io_uring_get_sqe(&m_ring);
        io_uring_prep_fsync(sqe, fd, 0);
        sqe->user_data = 2;
        io_uring_submit(&m_ring);

        bool succeeded = true;
        for (auto i = 0u; i < PARAGRAPH_SIZE; ++i) {
          io_uring_wait_cqe(&m_ring, &cqe);
          succeeded = succeeded && (cqe->res == size);
          io_uring_cqe_seen(&m_ring, cqe);
        }

        if (succeeded) {
          io_uring_wait_cqe(&m_ring, &cqe);
          succeeded = succeeded && (cqe->res == 0);
          io_uring_cqe_seen(&m_ring, cqe);
          // if (fsync_succeeded) break;
        }
      }
    }
  }

  close(fd);
  free(buf);
  io_uring_queue_exit(&m_ring);
}

BENCHMARK(BM_uring)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime()
    ->Args({1, 16})
    ->Args({2, 8})
    ->Args({4, 4})
    ->Args({8, 2})
    ->Args({16, 1});

BENCHMARK(BM_uring)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime()
    ->Args({1, 8})
    ->Args({2, 4})
    ->Args({4, 2})
    ->Args({8, 1});

BENCHMARK(BM_uring)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime()
    ->Args({1, 4})
    ->Args({2, 2})
    ->Args({4, 1});

BENCHMARK(BM_uring)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime()
    ->Args({1, 2})
    ->Args({1, 2});

BENCHMARK(BM_uring)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime()
    ->Args({1, 1});
```

Machine specification:

```
Running ./Release/test/io_uring/uring_benchmark
Run on (8 X 4358.57 MHz CPU s)
CPU Caches:
  L1 Data 32 KiB (x4)
  L1 Instruction 32 KiB (x4)
  L2 Unified 256 KiB (x4)
  L3 Unified 8192 KiB (x1)
Load Average: 0.75, 1.09, 0.95
```

### Result

In `BM_uring/x/y/real_time`, x represents the number of API calls for a single
batch, and y represents the number of packets in a single submission.

```
-------------------------------------------------------------------
Benchmark                         Time             CPU   Iterations
-------------------------------------------------------------------
BM_uring/1/128/real_time        799 ms          348 ms            1
BM_uring/2/64/real_time         464 ms          180 ms            2
BM_uring/4/32/real_time         283 ms         88.3 ms            3
BM_uring/8/16/real_time         193 ms         46.0 ms            4
BM_uring/16/8/real_time         148 ms         26.9 ms            5
BM_uring/32/4/real_time         112 ms         16.1 ms            6
BM_uring/64/2/real_time        80.9 ms         15.3 ms            8
BM_uring/128/1/real_time     **57.2 ms**       23.9 ms           12
-------------------------------------------------------------------
BM_uring/1/64/real_time         923 ms          350 ms            1
BM_uring/2/32/real_time         571 ms          179 ms            1
BM_uring/4/16/real_time         392 ms         91.5 ms            2
BM_uring/8/8/real_time          272 ms         46.6 ms            2
BM_uring/16/4/real_time         219 ms         28.0 ms            3
BM_uring/32/2/real_time         162 ms         22.4 ms            4
BM_uring/64/1/real_time       **116 ms**       27.7 ms            6
-------------------------------------------------------------------
BM_uring/1/32/real_time        1133 ms          356 ms            1
BM_uring/2/16/real_time         812 ms          185 ms            1
BM_uring/4/8/real_time          612 ms         95.2 ms            1
BM_uring/8/4/real_time          450 ms         50.8 ms            2
BM_uring/16/2/real_time         322 ms         31.5 ms            2
BM_uring/32/1/real_time       **238 ms**       33.4 ms            3
-------------------------------------------------------------------
BM_uring/1/16/real_time        1654 ms          372 ms            1
BM_uring/2/8/real_time         1213 ms          187 ms            1
BM_uring/4/4/real_time          911 ms          101 ms            1
BM_uring/8/2/real_time          659 ms         56.3 ms            1
BM_uring/16/1/real_time       **476 ms**       48.3 ms            2
--------------------------------------------------------------------
BM_uring/1/8/real_time         2437 ms          378 ms            1
BM_uring/2/4/real_time         1844 ms          197 ms            1
BM_uring/4/2/real_time         1310 ms          109 ms            1
BM_uring/8/1/real_time        **936 ms**       67.1 ms            1
--------------------------------------------------------------------
BM_uring/1/4/real_time         3668 ms          390 ms            1
BM_uring/2/2/real_time         2594 ms          206 ms            1
BM_uring/4/1/real_time       **1889 ms**        126 ms            1
-------------------------------------------------------------------
```


```
-------------------------------------------------------------------
Benchmark                         Time             CPU   Iterations
-------------------------------------------------------------------
BM_uring/128/1/real_time     **57.2 ms**       23.9 ms           12
BM_uring/64/1/real_time       **116 ms**       27.7 ms            6
BM_uring/32/1/real_time       **238 ms**       33.4 ms            3
BM_uring/16/1/real_time       **476 ms**       48.3 ms            2
BM_uring/8/1/real_time        **936 ms**       67.1 ms            1
BM_uring/4/1/real_time       **1889 ms**        126 ms            1
-------------------------------------------------------------------
```

If I handle requests with multiple threads, I expect that we will see better
results and the optimal value of y would be greater than 1.

### Conclusion

Wenn it comes to using `io_uring`, there are three important things to be aware of.

1. Writing number of request as a single batch is faster overall.
2. To garantee reliable update, single submit on sqe should be constituted with;
   1. several batchs with `IOSQE_IO_HARDLINK_BIT`,
   2. a following `IORING_OP_FSYNC` request,
   3. and a following summit.

I tried to program all of these without coroutine, and it was a nightmare.
Coroutines can easily handle all sorts of batching multiple API calls and checking completion on different queues also.

## Final Design

1. Create a coroutine for `push_back` and `erase` and save it as a `batch_coroutine`.
2. The coroutine should allocate a memory chunk aligned by 4KB to store a batch on a coroutine heap. It should be 'moved', not copied.
3. A single batch can have multiple requests. The size of the batch may vary, or it could be fixed by the number of API calls, but it will always occupy some amount of disk space, multiples of 4KB. Each memory chunk has its own header and stores the number of packets, total bytes, etc.
4. Every API call creates a coroutine named `batch` if one does not already exist.
5. Every API call resumes the `batch` coroutine. Information should be sent by the `resume()` interface of the coroutine. The coroutine appends its request to the memory chunk.
6. If the batch reaches the size limit, it should be submitted with `io_uring` and *moved* to the `coroutine_queue`, so `batch_coroutine` should be well emptied.
7. A thread pool will inspect the `coroutine_queue`. If it is not empty, threads will take coroutines and resume those.
8. The completion queue will be in order for a single submission due to the `IQSQE_IO_HARDLINK_BIT`. We can also wait for the completion of the write request and the subsequent fsync request.
9. Check if the request is handled properly with the response and retry if necessary. Write request must return its written byte information on its response in completion queue. FSYNC request will give us 0 as its response if everything goes well. The coroutine knows which packet it tried to write. This 'rewriting' must not be disturbed by other request submissions to guarantee reliability.
10. If a coroutine finishes its task, it should be removed from the `coroutine_queue` on the background thread. The completion may happen in the order of submissions, but I need to confirm this through experimentation. Otherwise, just removing it could lead to problems.
11. Threads in thread pool should check the `batch_coroutine` if there are no more requests for a certain amount of time (e.g. 1, 10msec).
12. Every queue access should be mutually protected with a mutex.

With this design, we can defer every request with `coroutine_queue` and handle the failure of every submission appropriately.

