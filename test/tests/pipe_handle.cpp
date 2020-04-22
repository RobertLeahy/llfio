/* Integration test kernel for whether pipe handles work
(C) 2019 Niall Douglas <http://www.nedproductions.biz/> (2 commits)
File Created: Nov 2019


Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License in the accompanying file
Licence.txt or at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.


Distributed under the Boost Software License, Version 1.0.
    (See accompanying file Licence.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

#include "../test_kernel_decl.hpp"

#include <future>
#include <unordered_set>

static inline void TestBlockingPipeHandle()
{
  namespace llfio = LLFIO_V2_NAMESPACE;
  auto readerthread = std::async([] {  // This immediately blocks in blocking mode
    llfio::pipe_handle reader = llfio::pipe_handle::pipe_create("llfio-pipe-handle-test").value();
    llfio::byte buffer[64];
    auto read = reader.read(0, {{buffer, 64}}).value();
    BOOST_REQUIRE(read == 5);
    BOOST_CHECK(0 == memcmp(buffer, "hello", 5));
    reader.close().value();
  });
  auto begin = std::chrono::steady_clock::now();
  while(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count() < 100)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if(std::future_status::ready == readerthread.wait_for(std::chrono::seconds(0)))
  {
    readerthread.get();  // rethrow exception
  }
  llfio::pipe_handle writer;
  begin = std::chrono::steady_clock::now();
  while(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count() < 1000)
  {
    auto r = llfio::pipe_handle::pipe_open("llfio-pipe-handle-test");
    if(r)
    {
      writer = std::move(r.value());
      break;
    }
  }
  BOOST_REQUIRE(writer.is_valid());
  auto written = writer.write(0, {{(const llfio::byte *) "hello", 5}}).value();
  BOOST_REQUIRE(written == 5);
  writer.barrier().value();
  writer.close().value();
  readerthread.get();
}

static inline void TestNonBlockingPipeHandle()
{
  namespace llfio = LLFIO_V2_NAMESPACE;
  llfio::pipe_handle reader = llfio::pipe_handle::pipe_create("llfio-pipe-handle-test", llfio::pipe_handle::caching::all, llfio::pipe_handle::flag::multiplexable).value();
  llfio::byte buffer[64];
  {  // no writer, so non-blocking read should time out
    auto read = reader.read(0, {{buffer, 64}}, std::chrono::milliseconds(0));
    BOOST_REQUIRE(read.has_error());
    BOOST_REQUIRE(read.error() == llfio::errc::timed_out);
  }
  {  // no writer, so blocking read should time out
    auto read = reader.read(0, {{buffer, 64}}, std::chrono::seconds(1));
    BOOST_REQUIRE(read.has_error());
    BOOST_REQUIRE(read.error() == llfio::errc::timed_out);
  }
  llfio::pipe_handle writer = llfio::pipe_handle::pipe_open("llfio-pipe-handle-test", llfio::pipe_handle::caching::all, llfio::pipe_handle::flag::multiplexable).value();
  auto written = writer.write(0, {{(const llfio::byte *) "hello", 5}}).value();
  BOOST_REQUIRE(written == 5);
  // writer.barrier().value();  // would block until pipe drained by reader
  // writer.close().value();  // would cause all further reads to fail due to pipe broken
  auto read = reader.read(0, {{buffer, 64}}, std::chrono::milliseconds(0));
  BOOST_REQUIRE(read.value() == 5);
  BOOST_CHECK(0 == memcmp(buffer, "hello", 5));
  writer.barrier().value();  // must not block nor fail
  writer.close().value();
  reader.close().value();
}

#if LLFIO_ENABLE_TEST_IO_MULTIPLEXERS
static inline void TestMultiplexedPipeHandle()
{
  static constexpr size_t MAX_PIPES = 64;
  namespace llfio = LLFIO_V2_NAMESPACE;
  auto test_multiplexer = [](llfio::io_multiplexer_ptr multiplexer) {
    std::vector<llfio::pipe_handle> read_pipes, write_pipes;
    std::vector<size_t> received_for(MAX_PIPES);
    struct checking_receiver final : public llfio::io_multiplexer::io_operation_state_visitor
    {
      size_t myindex;
      std::unique_ptr<llfio::byte[]> io_state_ptr;
      std::vector<size_t> &received_for;
      union {
        llfio::byte _buffer[sizeof(size_t)];
        size_t _index;
      };
      llfio::pipe_handle::buffer_type buffer;
      llfio::io_multiplexer::io_operation_state *io_state{nullptr};

      checking_receiver(size_t _myindex, llfio::io_multiplexer_ptr &multiplexer, std::vector<size_t> &r)
          : myindex(_myindex)
          , io_state_ptr(std::make_unique<llfio::byte[]>(multiplexer->io_state_requirements().first))
          , received_for(r)
          , buffer(_buffer, sizeof(_buffer))
      {
        memset(_buffer, 0, sizeof(_buffer));
      }
      checking_receiver(const checking_receiver &) = delete;
      checking_receiver(checking_receiver &&o) = default;
      checking_receiver &operator=(const checking_receiver &) = delete;
      checking_receiver &operator=(checking_receiver &&) = default;
      ~checking_receiver()
      {
        if(io_state != nullptr)
        {
          if(!is_finished(io_state->current_state()))
          {
            abort();
          }
          io_state->~io_operation_state();
          io_state = nullptr;
        }
      }

      // Initiated the read
      llfio::result<void> read_begin(llfio::io_multiplexer_ptr &multiplexer, llfio::io_handle &h)
      {
        if(io_state != nullptr)
        {
          BOOST_REQUIRE(is_finished(io_state->current_state()));
          io_state->~io_operation_state();
          io_state = nullptr;
        }
        buffer = {_buffer, sizeof(_buffer)};
        OUTCOME_TRY(s, multiplexer->init_io_operation({io_state_ptr.get(), 4096 /*lies*/}, &h, this, {}, {}, llfio::pipe_handle::io_request<llfio::pipe_handle::buffers_type>({&buffer, 1}, 0)));
        io_state = s;
        return llfio::success();
      }

      // Called if the read did not complete immediately
      virtual void read_initiated(llfio::io_multiplexer::io_operation_state * /*state*/, llfio::io_operation_state_type /*former*/) override { std::cout << "   Pipe " << myindex << " will complete read later" << std::endl; }

      // Called when the read completes
      virtual void read_completed(llfio::io_multiplexer::io_operation_state * /*state*/, llfio::io_operation_state_type former, llfio::pipe_handle::io_result<llfio::pipe_handle::buffers_type> &&res) override
      {
        if(is_initialised(former))
        {
          std::cout << "   Pipe " << myindex << " read completes immediately" << std::endl;
        }
        else
        {
          std::cout << "   Pipe " << myindex << " read completes asynchronously" << std::endl;
        }
        BOOST_CHECK(res.has_value());
        if(res)
        {
          BOOST_REQUIRE(res.value().size() == 1);
          BOOST_CHECK(res.value()[0].data() == _buffer);
          BOOST_CHECK(res.value()[0].size() == sizeof(size_t));
          BOOST_REQUIRE(_index < MAX_PIPES);
          BOOST_CHECK(_index == myindex);
          received_for[_index]++;
        }
      }

      // Called when the state for the read can be disposed
      virtual void read_finished(llfio::io_multiplexer::io_operation_state * /*state*/, llfio::io_operation_state_type former) override
      {
        std::cout << "   Pipe " << myindex << " read finishes" << std::endl;
        BOOST_REQUIRE(former == llfio::io_operation_state_type::read_completed);
      }
    };
    std::vector<checking_receiver> async_reads;
    for(size_t n = 0; n < MAX_PIPES; n++)
    {
      auto ret = llfio::pipe_handle::anonymous_pipe(llfio::pipe_handle::caching::reads, llfio::pipe_handle::flag::multiplexable).value();
      ret.first.set_multiplexer(multiplexer.get()).value();
      async_reads.push_back(checking_receiver(n, multiplexer, received_for));
      read_pipes.push_back(std::move(ret.first));
      write_pipes.push_back(std::move(ret.second));
    }
    auto writerthread = std::async([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      for(size_t n = MAX_PIPES - 1; n < MAX_PIPES; n--)
      {
        auto r = write_pipes[n].write(0, {{(llfio::byte *) &n, sizeof(n)}});
        if(!r)
        {
          abort();
        }
      }
    });
    // Start the pipe reads. They cannot move in memory until complete
    for(size_t n = 0; n < MAX_PIPES; n++)
    {
      async_reads[n].read_begin(multiplexer, read_pipes[n]).value();
    }
    // Wait for all reads to complete
    for(size_t n = 0; n < MAX_PIPES; n++)
    {
      // Spin until this i/o completes
      for(;;)
      {
        auto state = multiplexer->check_io_operation(async_reads[n].io_state);
        if(is_completed(state) || is_finished(state))
        {
          break;
        }
      }
    }
    for(size_t n = 0; n < MAX_PIPES; n++)
    {
      BOOST_CHECK(received_for[n] == 1);
    }
    // Wait for all reads to finish
    for(size_t n = 0; n < MAX_PIPES; n++)
    {
      llfio::io_operation_state_type state ;
      while(!is_finished(state = async_reads[n].io_state->current_state()))
      {
        multiplexer->check_for_any_completed_io().value();
      }
    }
    writerthread.get();
  };
#ifdef _WIN32
  std::cout << "\nSingle threaded IOCP, immediate completions:\n";
  test_multiplexer(llfio::test::multiplexer_win_iocp(1, false).value());
  std::cout << "\nSingle threaded IOCP, reactor completions:\n";
  test_multiplexer(llfio::test::multiplexer_win_iocp(1, true).value());
  std::cout << "\nMultithreaded IOCP, immediate completions:\n";
  test_multiplexer(llfio::test::multiplexer_win_iocp(2, false).value());
  std::cout << "\nMultithreaded IOCP, reactor completions:\n";
  test_multiplexer(llfio::test::multiplexer_win_iocp(2, true).value());
#else
#error Not implemented yet
#endif
}

#if LLFIO_ENABLE_COROUTINES && 0
static inline void TestCoroutinedPipeHandle()
{
  static constexpr size_t MAX_PIPES = 70;
  namespace llfio = LLFIO_V2_NAMESPACE;
  struct io_visitor
  {
    using container_type = std::unordered_set<llfio::io_awaitable<llfio::async_read, io_visitor> *>;
    container_type &c;
    void await_suspend(container_type::value_type i) { c.insert(i); }
    void await_resume(container_type::value_type i) { c.erase(i); }
  };
  io_visitor::container_type io_pending;
  struct coroutine
  {
    llfio::pipe_handle read_pipe, write_pipe;
    size_t received_for{0};

    explicit coroutine(llfio::pipe_handle &&r, llfio::pipe_handle &&w)
        : read_pipe(std::move(r))
        , write_pipe(std::move(w))
    {
    }
    llfio::eager<llfio::result<void>> operator()(io_visitor::container_type &io_pending)
    {
      union {
        llfio::byte _buffer[sizeof(size_t)];
        size_t _index;
      };
      llfio::pipe_handle::buffer_type buffer;
      for(;;)
      {
        buffer = {_buffer, sizeof(_buffer)};
        // This will never return if the coroutine gets cancelled
        auto r = co_await read_pipe.co_read(io_visitor{io_pending}, {{buffer}, 0});
        if(!r)
        {
          co_return r.error();
        }
        BOOST_CHECK(r.value().size() == 1);
        BOOST_CHECK(r.value()[0].size() == sizeof(_buffer));
        ++received_for;
      }
    }
  };
  std::vector<coroutine> coroutines;
  auto multiplexer = llfio::this_thread::multiplexer();
  for(size_t n = 0; n < MAX_PIPES; n++)
  {
    auto ret = llfio::pipe_handle::anonymous_pipe(llfio::pipe_handle::caching::reads, llfio::pipe_handle::flag::multiplexable).value();
    ret.first.set_multiplexer(multiplexer).value();
    coroutines.push_back(coroutine(std::move(ret.first), std::move(ret.second)));
  }
  // Start the coroutines, all of whom will begin a read and then suspend
  std::vector<llfio::optional<llfio::eager<llfio::result<void>>>> states(MAX_PIPES);
  for(size_t n = 0; n < MAX_PIPES; n++)
  {
    states[n].emplace(coroutines[n](io_pending));
  }
  // Write to all the pipes, then pump coroutine resumption until all completions done
  for(size_t i = 0; i < 10; i++)
  {
    for(size_t n = MAX_PIPES - 1; n < MAX_PIPES; n--)
    {
      coroutines[n].write_pipe.write(0, {{(llfio::byte *) &i, sizeof(i)}}).value();
    }
    // Take a copy of all pending i/o
    std::vector<io_visitor::container_type::value_type> copy(io_pending.begin(), io_pending.end());
    for(;;)
    {
      // Manually check if an i/o completion is ready, avoiding any syscalls
      bool need_to_poll = true;
      for(auto it = copy.begin(); it != copy.end();)
      {
        if((*it)->await_ready())
        {
          need_to_poll = false;
          it = copy.erase(it);
          std::cout << "Completed an i/o without syscall" << std::endl;
        }
        else
          ++it;
      }
      if(need_to_poll)
      {
        // Have the kernel tell me when an i/o completion is ready
        auto r = multiplexer->complete_io();
        BOOST_CHECK(r.value() != 0);
        if(r.value() < 0)
        {
          for(size_t n = 0; n < MAX_PIPES; n++)
          {
            BOOST_CHECK(coroutines[n].received_for == i + 1);
          }
          break;
        }
      }
    }
  }
  // Rethrow any failures
  for(size_t n = 0; n < MAX_PIPES; n++)
  {
    if(states[n]->await_ready())
    {
      states[n]->await_resume().value();
    }
  }
  // Destruction of coroutines when they are suspended must work.
  // This will cancel any pending i/o and immediately exit the
  // coroutines
  states.clear();
  // Now clear all the coroutines
  coroutines.clear();
}
#endif
#endif

KERNELTEST_TEST_KERNEL(integration, llfio, pipe_handle, blocking, "Tests that blocking llfio::pipe_handle works as expected", TestBlockingPipeHandle())
KERNELTEST_TEST_KERNEL(integration, llfio, pipe_handle, nonblocking, "Tests that nonblocking llfio::pipe_handle works as expected", TestNonBlockingPipeHandle())
#if LLFIO_ENABLE_TEST_IO_MULTIPLEXERS
KERNELTEST_TEST_KERNEL(integration, llfio, pipe_handle, multiplexed, "Tests that multiplexed llfio::pipe_handle works as expected", TestMultiplexedPipeHandle())
#if LLFIO_ENABLE_COROUTINES && 0
KERNELTEST_TEST_KERNEL(integration, llfio, pipe_handle, coroutined, "Tests that coroutined llfio::pipe_handle works as expected", TestCoroutinedPipeHandle())
#endif
#endif