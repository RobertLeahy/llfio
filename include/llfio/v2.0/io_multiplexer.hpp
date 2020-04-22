/* Multiplex file i/o
(C) 2019-2020 Niall Douglas <http://www.nedproductions.biz/> (9 commits)
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

#ifndef LLFIO_IO_MULTIPLEXER_H
#define LLFIO_IO_MULTIPLEXER_H

//#define LLFIO_DEBUG_PRINT
//#define LLFIO_ENABLE_TEST_IO_MULTIPLEXERS 1

#include "handle.hpp"

#include <memory>  // for unique_ptr and shared_ptr

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)  // dll interface
#endif

LLFIO_V2_NAMESPACE_EXPORT_BEGIN

class io_handle;

//! The possible states of the i/o operation
enum class io_operation_state_type
{
  unknown,
  read_initialised,
  read_initiated,
  read_completed,
  read_finished,
  write_initialised,
  write_initiated,
  barrier_initialised,
  barrier_initiated,
  write_or_barrier_completed,
  write_or_barrier_finished
};
//! True if the i/o operation state is initialised
constexpr inline bool is_initialised(io_operation_state_type s) noexcept
{
  switch(s)
  {
  case io_operation_state_type::unknown:
  case io_operation_state_type::read_initiated:
  case io_operation_state_type::read_completed:
  case io_operation_state_type::read_finished:
  case io_operation_state_type::write_initiated:
  case io_operation_state_type::write_or_barrier_completed:
  case io_operation_state_type::write_or_barrier_finished:
  case io_operation_state_type::barrier_initiated:
    return false;
  case io_operation_state_type::read_initialised:
  case io_operation_state_type::write_initialised:
  case io_operation_state_type::barrier_initialised:
    return true;
  }
  return false;
}
//! True if the i/o operation state is initiated
constexpr inline bool is_initiated(io_operation_state_type s) noexcept
{
  switch(s)
  {
  case io_operation_state_type::unknown:
  case io_operation_state_type::read_initialised:
  case io_operation_state_type::read_completed:
  case io_operation_state_type::read_finished:
  case io_operation_state_type::write_initialised:
  case io_operation_state_type::barrier_initialised:
  case io_operation_state_type::write_or_barrier_completed:
  case io_operation_state_type::write_or_barrier_finished:
    return false;
  case io_operation_state_type::read_initiated:
  case io_operation_state_type::write_initiated:
  case io_operation_state_type::barrier_initiated:
    return true;
  }
  return false;
}
//! True if the i/o operation state is completed
constexpr inline bool is_completed(io_operation_state_type s) noexcept
{
  switch(s)
  {
  case io_operation_state_type::unknown:
  case io_operation_state_type::read_initialised:
  case io_operation_state_type::read_initiated:
  case io_operation_state_type::read_finished:
  case io_operation_state_type::write_initialised:
  case io_operation_state_type::write_initiated:
  case io_operation_state_type::barrier_initialised:
  case io_operation_state_type::barrier_initiated:
  case io_operation_state_type::write_or_barrier_finished:
    return false;
  case io_operation_state_type::read_completed:
  case io_operation_state_type::write_or_barrier_completed:
    return true;
  }
  return false;
}
//! True if the i/o operation state is finished
constexpr inline bool is_finished(io_operation_state_type s) noexcept
{
  switch(s)
  {
  case io_operation_state_type::unknown:
  case io_operation_state_type::read_initialised:
  case io_operation_state_type::read_initiated:
  case io_operation_state_type::read_completed:
  case io_operation_state_type::write_initialised:
  case io_operation_state_type::write_initiated:
  case io_operation_state_type::barrier_initialised:
  case io_operation_state_type::barrier_initiated:
  case io_operation_state_type::write_or_barrier_completed:
    return false;
  case io_operation_state_type::read_finished:
  case io_operation_state_type::write_or_barrier_finished:
    return true;
  }
  return false;
}

/*! \class io_multiplexer
\brief A multiplexer of byte-orientated i/o.

LLFIO does not provide out-of-the-box multiplexing of byte i/o, however it does provide the ability
to create `io_handle` instances with the `handle::flag::multiplexable` set. With that flag set, the
following LLFIO classes change how they create handles with the kernel:

<table>
<tr><th>LLFIO i/o class<th>POSIX<th>Windows
<tr><td><code>directory_handle</code><td>No effect<td>Creates `HANDLE` as `OVERLAPPED`
<tr><td><code>file_handle</code><td>No effect<td>Creates `HANDLE` as `OVERLAPPED`
<tr><td><code>map_handle</code><td>No effect<td>No effect
<tr><td><code>mapped_file_handle</code><td>No effect<td>Creates `HANDLE` as `OVERLAPPED`, but i/o is to map not file
<tr><td><code>pipe_handle</code><td>Creates file descriptor as non-blocking<td>Creates `HANDLE` as `OVERLAPPED`
<tr><td><code>section_handle</code><td>No effect<td>Creates `HANDLE` as `OVERLAPPED`
<tr><td><code>symlink_handle</code><td>No effect<td>Creates `HANDLE` as `OVERLAPPED`
</table>

If the i/o handle's multiplexer pointer is not null, the multiplexer instance is invoked to implement
`io_handle::read()`, `io_handle::write()` and `io_handle::barrier()` by constructing an i/o
operation state on the stack, calling `.init_io_operation()` followed by `.flush_inited_io_operations()`,
and then spinning on `.check_io_operation()` and `.check_for_any_completed_io()` with the deadline
specified to the original blocking operation.

If the i/o handle's multiplexer pointer is null, `io_handle::read()`, `io_handle::write()` and
`io_handle::barrier()` all use virtually overridable implementations. The default implementations
emulate blocking semantics using the kernel's i/o poll function (literally `poll()` on POSIX,
`NtWaitForSingleObject()` on Windows) to sleep the thread until at least one byte of i/o occurs, or
the deadline specified is exceeded. This, obviously enough, can double the number of kernel syscalls
done per i/o, so using handles with the `handle::flag::multiplexable` flag set is not wise unless
you really need non-infinite deadline i/o.
*/
class LLFIO_DECL io_multiplexer : public handle
{
  struct _empty_t
  {
  };

public:
  using path_type = handle::path_type;
  using extent_type = handle::extent_type;
  using size_type = handle::size_type;
  using mode = handle::mode;
  using creation = handle::creation;
  using caching = handle::caching;
  using flag = handle::flag;

  //! The kinds of write reordering barrier which can be performed.
  enum class barrier_kind : uint8_t
  {
    nowait_data_only,  //!< Barrier data only, non-blocking. This is highly optimised on NV-DIMM storage, but consider using `nvram_barrier()` for even better performance.
    wait_data_only,    //!< Barrier data only, block until it is done. This is highly optimised on NV-DIMM storage, but consider using `nvram_barrier()` for even better performance.
    nowait_all,        //!< Barrier data and the metadata to retrieve it, non-blocking.
    wait_all           //!< Barrier data and the metadata to retrieve it, block until it is done.
  };

  //! The scatter buffer type used by this handle. Guaranteed to be `TrivialType` and `StandardLayoutType`.
  //! Try to make address and length 64 byte, or ideally, `page_size()` aligned where possible.
  struct buffer_type
  {
    //! Type of the pointer to memory.
    using pointer = byte *;
    //! Type of the pointer to memory.
    using const_pointer = const byte *;
    //! Type of the iterator to memory.
    using iterator = byte *;
    //! Type of the iterator to memory.
    using const_iterator = const byte *;
    //! Type of the length of memory.
    using size_type = size_t;

    //! Default constructor
    buffer_type() = default;
    //! Constructor
    constexpr buffer_type(pointer data, size_type len) noexcept
        : _data(data)
        , _len(len)
    {
    }
    //! Constructor
    constexpr buffer_type(span<byte> s) noexcept
        : _data(s.data())
        , _len(s.size())
    {
    }
    buffer_type(const buffer_type &) = default;
    buffer_type(buffer_type &&) = default;
    buffer_type &operator=(const buffer_type &) = default;
    buffer_type &operator=(buffer_type &&) = default;
    ~buffer_type() = default;

    // Emulation of this being a span<byte> in the TS

    //! Returns the address of the bytes for this buffer
    constexpr pointer data() noexcept { return _data; }
    //! Returns the address of the bytes for this buffer
    constexpr const_pointer data() const noexcept { return _data; }
    //! Returns the number of bytes in this buffer
    constexpr size_type size() const noexcept { return _len; }

    //! Returns an iterator to the beginning of the buffer
    constexpr iterator begin() noexcept { return _data; }
    //! Returns an iterator to the beginning of the buffer
    constexpr const_iterator begin() const noexcept { return _data; }
    //! Returns an iterator to the beginning of the buffer
    constexpr const_iterator cbegin() const noexcept { return _data; }
    //! Returns an iterator to after the end of the buffer
    constexpr iterator end() noexcept { return _data + _len; }
    //! Returns an iterator to after the end of the buffer
    constexpr const_iterator end() const noexcept { return _data + _len; }
    //! Returns an iterator to after the end of the buffer
    constexpr const_iterator cend() const noexcept { return _data + _len; }

  private:
    friend constexpr inline void _check_iovec_match();
    pointer _data;
    size_type _len;
  };

  //! The gather buffer type used by this handle. Guaranteed to be `TrivialType` and `StandardLayoutType`.
  //! Try to make address and length 64 byte, or ideally, `page_size()` aligned where possible.
  struct const_buffer_type
  {
    //! Type of the pointer to memory.
    using pointer = const byte *;
    //! Type of the pointer to memory.
    using const_pointer = const byte *;
    //! Type of the iterator to memory.
    using iterator = const byte *;
    //! Type of the iterator to memory.
    using const_iterator = const byte *;
    //! Type of the length of memory.
    using size_type = size_t;

    //! Default constructor
    const_buffer_type() = default;
    //! Constructor
    constexpr const_buffer_type(pointer data, size_type len) noexcept
        : _data(data)
        , _len(len)
    {
    }
    //! Constructor
    constexpr const_buffer_type(span<const byte> s) noexcept
        : _data(s.data())
        , _len(s.size())
    {
    }
    //! Converting constructor from non-const buffer type
    constexpr const_buffer_type(buffer_type b) noexcept
        : _data(b.data())
        , _len(b.size())
    {
    }
    //! Converting constructor from non-const buffer type
    constexpr const_buffer_type(span<byte> s) noexcept
        : _data(s.data())
        , _len(s.size())
    {
    }
    const_buffer_type(const const_buffer_type &) = default;
    const_buffer_type(const_buffer_type &&) = default;
    const_buffer_type &operator=(const const_buffer_type &) = default;
    const_buffer_type &operator=(const_buffer_type &&) = default;
    ~const_buffer_type() = default;

    // Emulation of this being a span<byte> in the TS

    //! Returns the address of the bytes for this buffer
    constexpr pointer data() noexcept { return _data; }
    //! Returns the address of the bytes for this buffer
    constexpr const_pointer data() const noexcept { return _data; }
    //! Returns the number of bytes in this buffer
    constexpr size_type size() const noexcept { return _len; }

    //! Returns an iterator to the beginning of the buffer
    constexpr iterator begin() noexcept { return _data; }
    //! Returns an iterator to the beginning of the buffer
    constexpr const_iterator begin() const noexcept { return _data; }
    //! Returns an iterator to the beginning of the buffer
    constexpr const_iterator cbegin() const noexcept { return _data; }
    //! Returns an iterator to after the end of the buffer
    constexpr iterator end() noexcept { return _data + _len; }
    //! Returns an iterator to after the end of the buffer
    constexpr const_iterator end() const noexcept { return _data + _len; }
    //! Returns an iterator to after the end of the buffer
    constexpr const_iterator cend() const noexcept { return _data + _len; }

  private:
    pointer _data;
    size_type _len;
  };
#ifndef NDEBUG
  static_assert(std::is_trivial<buffer_type>::value, "buffer_type is not a trivial type!");
  static_assert(std::is_trivial<const_buffer_type>::value, "const_buffer_type is not a trivial type!");
  static_assert(std::is_standard_layout<buffer_type>::value, "buffer_type is not a standard layout type!");
  static_assert(std::is_standard_layout<const_buffer_type>::value, "const_buffer_type is not a standard layout type!");
#endif

  struct _registered_buffer_type : std::enable_shared_from_this<_registered_buffer_type>, span<byte>
  {
    using span<byte>::span;
    explicit _registered_buffer_type(span<byte> o)
        : span<byte>(o)
    {
    }
    _registered_buffer_type() = default;
    _registered_buffer_type(const _registered_buffer_type &) = default;
    _registered_buffer_type(_registered_buffer_type &&) = default;
    _registered_buffer_type &operator=(const _registered_buffer_type &) = default;
    _registered_buffer_type &operator=(_registered_buffer_type &&) = default;
    ~_registered_buffer_type() = default;
  };
  //! The registered buffer type used by this handle.
  using registered_buffer_type = std::shared_ptr<_registered_buffer_type>;

  //! The scatter buffers type used by this handle. Guaranteed to be `TrivialType` apart from construction, and `StandardLayoutType`.
  using buffers_type = span<buffer_type>;
  //! The gather buffers type used by this handle. Guaranteed to be `TrivialType` apart from construction, and `StandardLayoutType`.
  using const_buffers_type = span<const_buffer_type>;
#ifndef NDEBUG
  // Is trivial in all ways, except default constructibility
  static_assert(std::is_trivially_copyable<buffers_type>::value, "buffers_type is not trivially copyable!");
  // static_assert(std::is_trivially_assignable<buffers_type, buffers_type>::value, "buffers_type is not trivially assignable!");
  // static_assert(std::is_trivially_destructible<buffers_type>::value, "buffers_type is not trivially destructible!");
  // static_assert(std::is_trivially_copy_constructible<buffers_type>::value, "buffers_type is not trivially copy constructible!");
  // static_assert(std::is_trivially_move_constructible<buffers_type>::value, "buffers_type is not trivially move constructible!");
  // static_assert(std::is_trivially_copy_assignable<buffers_type>::value, "buffers_type is not trivially copy assignable!");
  // static_assert(std::is_trivially_move_assignable<buffers_type>::value, "buffers_type is not trivially move assignable!");
  static_assert(std::is_standard_layout<buffers_type>::value, "buffers_type is not a standard layout type!");
#endif

  //! The i/o request type used by this handle. Guaranteed to be `TrivialType` apart from construction, and `StandardLayoutType`.
  template <class T> struct io_request
  {
    T buffers{};
    extent_type offset{0};
    constexpr io_request() {}  // NOLINT (defaulting this breaks clang and GCC, so don't do it!)
    constexpr io_request(T _buffers, extent_type _offset)
        : buffers(std::move(_buffers))
        , offset(_offset)
    {
    }
  };
#ifndef NDEBUG
  // Is trivial in all ways, except default constructibility
  static_assert(std::is_trivially_copyable<io_request<buffers_type>>::value, "io_request<buffers_type> is not trivially copyable!");
  // static_assert(std::is_trivially_assignable<io_request<buffers_type>, io_request<buffers_type>>::value, "io_request<buffers_type> is not trivially assignable!");
  // static_assert(std::is_trivially_destructible<io_request<buffers_type>>::value, "io_request<buffers_type> is not trivially destructible!");
  // static_assert(std::is_trivially_copy_constructible<io_request<buffers_type>>::value, "io_request<buffers_type> is not trivially copy constructible!");
  // static_assert(std::is_trivially_move_constructible<io_request<buffers_type>>::value, "io_request<buffers_type> is not trivially move constructible!");
  // static_assert(std::is_trivially_copy_assignable<io_request<buffers_type>>::value, "io_request<buffers_type> is not trivially copy assignable!");
  // static_assert(std::is_trivially_move_assignable<io_request<buffers_type>>::value, "io_request<buffers_type> is not trivially move assignable!");
  static_assert(std::is_standard_layout<io_request<buffers_type>>::value, "io_request<buffers_type> is not a standard layout type!");
#endif
  //! The i/o result type used by this handle. Guaranteed to be `TrivialType` apart from construction.
  template <class T> struct io_result : public LLFIO_V2_NAMESPACE::result<T>
  {
    using Base = LLFIO_V2_NAMESPACE::result<T>;
    size_type _bytes_transferred{static_cast<size_type>(-1)};

#if defined(_MSC_VER) && !defined(__clang__)  // workaround MSVC parsing bug
    constexpr io_result()
        : Base()
    {
    }
    template <class... Args>
    constexpr io_result(Args &&... args)
        : Base(std::forward<Args>(args)...)
    {
    }
#else
    using Base::Base;
    io_result() = default;
#endif
    ~io_result() = default;
    io_result &operator=(io_result &&) = default;  // NOLINT
#if LLFIO_EXPERIMENTAL_STATUS_CODE
    io_result(const io_result &) = delete;
    io_result &operator=(const io_result &) = delete;
#else
    io_result(const io_result &) = default;
    io_result &operator=(const io_result &) = default;
#endif
    io_result(io_result &&) = default;  // NOLINT
    //! Returns bytes transferred
    size_type bytes_transferred() noexcept
    {
      if(_bytes_transferred == static_cast<size_type>(-1))
      {
        _bytes_transferred = 0;
        for(auto &i : this->value())
        {
          _bytes_transferred += i.size();
        }
      }
      return _bytes_transferred;
    }
  };
#if !defined(NDEBUG) && !LLFIO_EXPERIMENTAL_STATUS_CODE
  // Is trivial in all ways, except default constructibility
  static_assert(std::is_trivially_copyable<io_result<buffers_type>>::value, "io_result<buffers_type> is not trivially copyable!");
// static_assert(std::is_trivially_assignable<io_result<buffers_type>, io_result<buffers_type>>::value, "io_result<buffers_type> is not trivially assignable!");
// static_assert(std::is_trivially_destructible<io_result<buffers_type>>::value, "io_result<buffers_type> is not trivially destructible!");
// static_assert(std::is_trivially_copy_constructible<io_result<buffers_type>>::value, "io_result<buffers_type> is not trivially copy constructible!");
// static_assert(std::is_trivially_move_constructible<io_result<buffers_type>>::value, "io_result<buffers_type> is not trivially move constructible!");
// static_assert(std::is_trivially_copy_assignable<io_result<buffers_type>>::value, "io_result<buffers_type> is not trivially copy assignable!");
// static_assert(std::is_trivially_move_assignable<io_result<buffers_type>>::value, "io_result<buffers_type> is not trivially move assignable!");
//! \todo Why is io_result<buffers_type> not a standard layout type?
// static_assert(std::is_standard_layout<result<buffers_type>>::value, "result<buffers_type> is not a standard layout type!");
// static_assert(std::is_standard_layout<io_result<buffers_type>>::value, "io_result<buffers_type> is not a standard layout type!");
#endif

  using handle::handle;
  constexpr io_multiplexer() {}
  io_multiplexer(io_multiplexer &&) = default;
  io_multiplexer(const io_multiplexer &) = delete;
  io_multiplexer &operator=(io_multiplexer &&) = default;
  io_multiplexer &operator=(const io_multiplexer &) = delete;
  ~io_multiplexer() = default;

public:
  //! Implements `io_handle` registration. The bottom two bits of the returned value are set into `_v.behaviour`'s `_multiplexer_state_bit0` and `_multiplexer_state_bit`
  LLFIO_HEADERS_ONLY_VIRTUAL_SPEC result<uint8_t> do_io_handle_register(io_handle * /*unused*/) noexcept { return (uint8_t) 0; }
  //! Implements `io_handle` deregistration
  LLFIO_HEADERS_ONLY_VIRTUAL_SPEC result<void> do_io_handle_deregister(io_handle * /*unused*/) noexcept { return success(); }
  //! Implements `io_handle::max_buffers()`
  LLFIO_HEADERS_ONLY_VIRTUAL_SPEC size_t do_io_handle_max_buffers(const io_handle *h) const noexcept;
  //! Implements `io_handle::allocate_registered_buffer()`
  LLFIO_HEADERS_ONLY_VIRTUAL_SPEC result<registered_buffer_type> do_io_handle_allocate_registered_buffer(io_handle *h, size_t &bytes) noexcept;

  struct io_operation_state_visitor;
  /*! \brief An interface to a state for an i/o operation scheduled against an i/o multiplexer.

  You will need to ask the i/o multiplexer for how much storage, and alignment, is required to
  store one of these using `io_multiplexer::io_state_requirements()`. Be aware that for some
  i/o multiplexers, quite a lot of storage (e.g. > 1Kb for IOCP on Windows) may be required.
  You can either preallocate i/o operation states for later use, or use other determinism-maintaining
  tricks to avoid dynamic memory allocation for i/o operation states.

  When you construct one of these using `io_multiplexer::init_io_operation()`, you must
  pass in a pointer to a `io_operation_state_visitor`. This visitor will be called whenever
  the lifecycle for the i/o state is about to change (it is called just before
  `.current_state()` is changed, and with any per-state locks held).

  The lifecycle for one of these is as follows:

  1. i/o initialised. This is after `io_multiplexer::init_io_operation()` has been
  called to initialise the i/o operation state. You can now no longer relocate the i/o
  operation state in memory until the corresponding `*_finished()` visitor function is called.

  2. i/o initiated. One is notified of this by the call of the corresponding visitor
  `*_initiated()` function. This may occur in `io_multiplexer::init_io_operation()`,
  in `io_multiplexer::flush_inited_io_operations()`, or **never at all** if the i/o
  completed immediately.

  3. When the i/o completes, one is notified of the i/o's result by the call of the
  corresponding `*_completed()` visitor function. This can occur *at any time*, and can be called
  by **any** kernel thread, if the i/o multiplexer in use is used by multiple kernel threads.
  The completion functions are *usually* invoked by somebody calling `io_multiplexer::check_io_operation()`
  or `io_multiplexer::check_for_any_completed_io()`, but may also be called by an asynchronous system agent.

  4. The i/o operation state may still be in use by others. You must not relocate in memory the
  i/o operation state after `io_multiplexer::init_io_operation()` returns until the corresponding
  `*_finished()` visitor function is called.
  */
  struct io_operation_state
  {
    //! The i/o handle the i/o operation is upon
    io_handle *h{nullptr};
    //! The state visitor supplied when the operation was initialised
    io_operation_state_visitor *visitor{nullptr};

  protected:
    constexpr io_operation_state() {}
    constexpr io_operation_state(io_handle *_h, io_operation_state_visitor *_visitor)
        : h(_h)
        , visitor(_visitor)
    {
    }

  public:
    virtual ~io_operation_state() {}

    //! Used to retrieve the current state of the i/o operation
    virtual io_operation_state_type current_state() const noexcept = 0;
    //! After an i/o operation has finished, can be used to retrieve the result if the visitor did not.
    virtual io_result<buffers_type> get_completed_read() &&noexcept = 0;
    //! After an i/o operation has finished, can be used to retrieve the result if the visitor did not.
    virtual io_result<const_buffers_type> get_completed_write_or_barrier() &&noexcept = 0;
  };
  //! \brief Called by an i/o operation state to inform you of state change. Note that the i/o operation state lock is HELD during these calls!
  struct io_operation_state_visitor
  {
    virtual ~io_operation_state_visitor() {}
    virtual void read_initiated(io_operation_state * /*state*/, io_operation_state_type /*former*/) {}
    virtual void read_completed(io_operation_state * /*state*/, io_operation_state_type /*former*/, io_result<buffers_type> && /*res*/) {}
    virtual void read_finished(io_operation_state * /*state*/, io_operation_state_type /*former*/) {}
    virtual void write_initiated(io_operation_state * /*state*/, io_operation_state_type /*former*/) {}
    virtual void write_completed(io_operation_state * /*state*/, io_operation_state_type /*former*/, io_result<const_buffers_type> && /*res*/) {}
    virtual void barrier_initiated(io_operation_state * /*state*/, io_operation_state_type /*former*/) {}
    virtual void barrier_completed(io_operation_state * /*state*/, io_operation_state_type /*former*/, io_result<const_buffers_type> && /*res*/) {}
    virtual void write_or_barrier_finished(io_operation_state * /*state*/, io_operation_state_type /*former*/) {}
  };

protected:
  /*! \brief An unsynchronised i/o operation state.

  This implementation does NOT use atomics during access.
  */
  struct _unsynchronised_io_operation_state : public io_operation_state
  {
    struct _lock_impl_type
    {
      void lock() {}
      void unlock() {}
    };
    _lock_impl_type _lock;
    using _lock_guard = lock_guard<_lock_impl_type>;
    //! The current lifecycle state of this i/o operation
    io_operation_state_type state{io_operation_state_type::unknown};
    //! Variant storage
    union payload_t {
      //! Used for unknown state
      _empty_t empty;
      //! Storage for non-completed i/o
      struct noncompleted_t
      {
        //! The registered buffer to use for the i/o, if any
        registered_buffer_type base;
        //! The deadline to complete the i/o by, if any
        deadline d;
        //! Variant storage for the possible kinds of non-completed i/o
        union params_t {
          //! Storage for a read i/o, the buffers to fill.
          struct read_params_t
          {
            io_request<buffers_type> reqs;
          } read;
          //! Storage for a write i/o, the buffers to drain.
          struct write_params_t
          {
            io_request<const_buffers_type> reqs;
          } write;
          //! Storage for a barrier i/o, the buffers to flush.
          struct barrier_params_t
          {
            io_request<const_buffers_type> reqs;
            barrier_kind kind;
          } barrier;

          explicit params_t(io_request<buffers_type> reqs)
              : read{std::move(reqs)}
          {
          }
          explicit params_t(io_request<const_buffers_type> reqs)
              : write{std::move(reqs)}
          {
          }
          explicit params_t(io_request<const_buffers_type> reqs, barrier_kind kind)
              : barrier{std::move(reqs), kind}
          {
          }
        } params;

        noncompleted_t(registered_buffer_type &&b, deadline _d, io_request<buffers_type> reqs)
            : base(std::move(b))
            , d(_d)
            , params(std::move(reqs))
        {
        }
        noncompleted_t(registered_buffer_type &&b, deadline _d, io_request<const_buffers_type> reqs)
            : base(std::move(b))
            , d(_d)
            , params(std::move(reqs))
        {
        }
        noncompleted_t(registered_buffer_type &&b, deadline _d, io_request<const_buffers_type> reqs, barrier_kind kind)
            : base(std::move(b))
            , d(_d)
            , params(std::move(reqs), kind)
        {
        }
      } noncompleted;
      //! Storage for a completed read i/o, the buffers filled.
      io_result<buffers_type> completed_read;
      //! Storage for a completed write or barrier i/o, the buffers drained.
      io_result<const_buffers_type> completed_write_or_barrier;

      constexpr payload_t()
          : empty()
      {
      }
      ~payload_t() {}
      payload_t(registered_buffer_type &&b, deadline d, io_request<buffers_type> reqs)
          : noncompleted(std::move(b), d, std::move(reqs))
      {
      }
      payload_t(registered_buffer_type &&b, deadline d, io_request<const_buffers_type> reqs)
          : noncompleted(std::move(b), d, std::move(reqs))
      {
      }
      payload_t(registered_buffer_type &&b, deadline d, io_request<const_buffers_type> reqs, barrier_kind kind)
          : noncompleted(std::move(b), d, std::move(reqs), kind)
      {
      }
      explicit payload_t(io_result<buffers_type> &&res)
          : completed_read(std::move(res))
      {
      }
      explicit payload_t(io_result<const_buffers_type> &&res)
          : completed_write_or_barrier(std::move(res))
      {
      }
    } payload;

    //! Construct an unknown state
    constexpr _unsynchronised_io_operation_state() {}
    //! Construct a read operation state
    _unsynchronised_io_operation_state(io_handle *_h, io_operation_state_visitor *_v, registered_buffer_type &&b, deadline d, io_request<buffers_type> reqs)
        : io_operation_state(_h, _v)
        , state(io_operation_state_type::read_initialised)
        , payload(std::move(b), d, std::move(reqs))
    {
    }
    //! Construct a write operation state
    _unsynchronised_io_operation_state(io_handle *_h, io_operation_state_visitor *_v, registered_buffer_type &&b, deadline d, io_request<const_buffers_type> reqs)
        : io_operation_state(_h, _v)
        , state(io_operation_state_type::write_initialised)
        , payload(std::move(b), d, std::move(reqs))
    {
    }
    //! Construct a barrier operation state
    _unsynchronised_io_operation_state(io_handle *_h, io_operation_state_visitor *_v, registered_buffer_type &&b, deadline d, io_request<const_buffers_type> reqs, barrier_kind kind)
        : io_operation_state(_h, _v)
        , state(io_operation_state_type::barrier_initialised)
        , payload(std::move(b), d, std::move(reqs), kind)
    {
    }
    _unsynchronised_io_operation_state(const _unsynchronised_io_operation_state &) = delete;
    _unsynchronised_io_operation_state(_unsynchronised_io_operation_state &&) = delete;
    _unsynchronised_io_operation_state &operator=(const _unsynchronised_io_operation_state &) = delete;
    _unsynchronised_io_operation_state &operator=(_unsynchronised_io_operation_state &&) = delete;

    virtual ~_unsynchronised_io_operation_state() { clear_storage(); }

    //! Used to clear the storage in this operation state
    void clear_storage()
    {
      switch(state)
      {
      case io_operation_state_type::unknown:
        break;
      case io_operation_state_type::read_initialised:
      case io_operation_state_type::read_initiated:
        payload.noncompleted.base.~registered_buffer_type();
        payload.noncompleted.d.~deadline();
        payload.noncompleted.params.read.~read_params_t();
        break;
      case io_operation_state_type::read_completed:
      case io_operation_state_type::read_finished:
        payload.completed_read.~io_result<buffers_type>();
        break;
      case io_operation_state_type::write_initialised:
      case io_operation_state_type::write_initiated:
        payload.noncompleted.base.~registered_buffer_type();
        payload.noncompleted.d.~deadline();
        payload.noncompleted.params.write.~write_params_t();
        break;
      case io_operation_state_type::barrier_initialised:
      case io_operation_state_type::barrier_initiated:
        payload.noncompleted.base.~registered_buffer_type();
        payload.noncompleted.d.~deadline();
        payload.noncompleted.params.barrier.~barrier_params_t();
        break;
      case io_operation_state_type::write_or_barrier_completed:
      case io_operation_state_type::write_or_barrier_finished:
        payload.completed_write_or_barrier.~io_result<const_buffers_type>();
        break;
      }
      state = io_operation_state_type::unknown;
    }

    virtual io_operation_state_type current_state() const noexcept override { return state; }
    virtual io_result<buffers_type> get_completed_read() && noexcept override
    {
      io_result<buffers_type> ret(std::move(payload.completed_read));
      return ret;
    }
    virtual io_result<const_buffers_type> get_completed_write_or_barrier() && noexcept override
    {
      io_result<const_buffers_type> ret(std::move(payload.completed_write_or_barrier));
      return ret;
    }

    virtual void read_initiated()
    {
      if(state == io_operation_state_type::read_initialised)
      {
        if(this->visitor != nullptr)
        {
          this->visitor->read_initiated(this, state);
        }
        state = io_operation_state_type::read_initiated;
      }
    }
    virtual void read_completed(io_result<buffers_type> &&res)
    {
      if(state == io_operation_state_type::read_initialised || state == io_operation_state_type::read_initiated)
      {
        clear_storage();
        new(&payload.completed_read) io_result<buffers_type>(std::move(res));
        if(this->visitor != nullptr)
        {
          this->visitor->read_completed(this, state, std::move(payload.completed_read));
        }
        state = io_operation_state_type::read_completed;
      }
    }
    virtual void read_finished()
    {
      if(state == io_operation_state_type::read_completed)
      {
        if(this->visitor != nullptr)
        {
          this->visitor->read_finished(this, state);
        }
        state = io_operation_state_type::read_finished;
      }
    }
    virtual void write_initiated()
    {
      if(state == io_operation_state_type::write_initialised)
      {
        if(this->visitor != nullptr)
        {
          this->visitor->write_initiated(this, state);
        }
        state = io_operation_state_type::write_initiated;
      }
    }
    virtual void write_completed(io_result<const_buffers_type> &&res)
    {
      if(state == io_operation_state_type::write_initialised || state == io_operation_state_type::write_initiated)
      {
        clear_storage();
        new(&payload.completed_write_or_barrier) io_result<const_buffers_type>(std::move(res));
        if(this->visitor != nullptr)
        {
          this->visitor->write_completed(this, state, std::move(payload.completed_write_or_barrier));
        }
        state = io_operation_state_type::write_or_barrier_completed;
      }
    }
    virtual void barrier_initiated()
    {
      if(state == io_operation_state_type::barrier_initialised)
      {
        if(this->visitor != nullptr)
        {
          this->visitor->barrier_initiated(this, state);
        }
        state = io_operation_state_type::barrier_initiated;
      }
    }
    virtual void barrier_completed(io_result<const_buffers_type> &&res)
    {
      if(state == io_operation_state_type::barrier_initialised || state == io_operation_state_type::barrier_initiated)
      {
        clear_storage();
        new(&payload.completed_write_or_barrier) io_result<const_buffers_type>(std::move(res));
        if(this->visitor != nullptr)
        {
          this->visitor->barrier_completed(this, state, std::move(payload.completed_write_or_barrier));
        }
        state = io_operation_state_type::write_or_barrier_completed;
      }
    }
    virtual void write_or_barrier_finished()
    {
      if(state == io_operation_state_type::write_or_barrier_completed)
      {
        if(this->visitor != nullptr)
        {
          this->visitor->write_or_barrier_finished(this, state);
        }
        state = io_operation_state_type::write_or_barrier_finished;
      }
    }
  };
  /*! \brief A synchronised i/o operation state.

  This implementation uses an atomic to synchronise access during the `*_completed()`,
  `*_finished()` and `current_state()` member functions, thus making it suitable for
  use across threads.
  */
  struct _synchronised_io_operation_state : public _unsynchronised_io_operation_state
  {
    using _lock_guard = lock_guard<spinlock>;
    mutable spinlock _lock;

    _synchronised_io_operation_state() = default;
    using _unsynchronised_io_operation_state::_unsynchronised_io_operation_state;
    virtual io_operation_state_type current_state() const noexcept override
    {
      _lock_guard g(this->_lock);
      return _unsynchronised_io_operation_state::current_state();
    }
    virtual io_result<buffers_type> get_completed_read() && noexcept override
    {
      _lock_guard g(this->_lock);
      return std::move(*this)._unsynchronised_io_operation_state::get_completed_read();
    }
    virtual io_result<const_buffers_type> get_completed_write_or_barrier() && noexcept override
    {
      _lock_guard g(this->_lock);
      return std::move(*this)._unsynchronised_io_operation_state::get_completed_write_or_barrier();
    }

    virtual void read_initiated() override
    {
      _lock_guard g(this->_lock);
      return _unsynchronised_io_operation_state::read_initiated();
    }
    virtual void read_completed(io_result<buffers_type> &&res) override
    {
      _lock_guard g(this->_lock);
      return _unsynchronised_io_operation_state::read_completed(std::move(res));
    }
    virtual void read_finished() override
    {
      _lock_guard g(this->_lock);
      return _unsynchronised_io_operation_state::read_finished();
    }
    virtual void write_initiated() override
    {
      _lock_guard g(this->_lock);
      return _unsynchronised_io_operation_state::write_initiated();
    }
    virtual void write_completed(io_result<const_buffers_type> &&res) override
    {
      _lock_guard g(this->_lock);
      return _unsynchronised_io_operation_state::write_completed(std::move(res));
    }
    virtual void barrier_initiated() override
    {
      _lock_guard g(this->_lock);
      return _unsynchronised_io_operation_state::barrier_initiated();
    }
    virtual void barrier_completed(io_result<const_buffers_type> &&res) override
    {
      _lock_guard g(this->_lock);
      return _unsynchronised_io_operation_state::barrier_completed(std::move(res));
    }
    virtual void write_or_barrier_finished() override
    {
      _lock_guard g(this->_lock);
      return _unsynchronised_io_operation_state::write_or_barrier_finished();
    }
  };

public:
  //! Returns the number of bytes, and alignment required, for an `io_operation_state` for this multiplexer
  virtual std::pair<size_t, size_t> io_state_requirements() noexcept = 0;

  /*! \brief Constructs either a `unsynchronised_io_operation_state` or a `synchronised_io_operation_state`
  for a read operation into the storage provided, possibly initiating the i/o as well. The storage must
  meet the requirements from `state_requirements()`.
  */
  virtual result<io_operation_state *> init_io_operation(span<byte> storage, io_handle *_h, io_operation_state_visitor *_visitor, registered_buffer_type &&b, deadline d, io_request<buffers_type> reqs) noexcept = 0;

  /*! \brief Constructs either a `unsynchronised_io_operation_state` or a `synchronised_io_operation_state`
  for a write operation into the storage provided, possibly initiating the i/o as well. The storage must
  meet the requirements from `state_requirements()`.
  */
  virtual result<io_operation_state *> init_io_operation(span<byte> storage, io_handle *_h, io_operation_state_visitor *_visitor, registered_buffer_type &&b, deadline d, io_request<const_buffers_type> reqs) noexcept = 0;

  /*! \brief Constructs either a `unsynchronised_io_operation_state` or a `synchronised_io_operation_state`
  for a barrier operation into the storage provided, possibly initiating the i/o as well. The storage must
  meet the requirements from `state_requirements()`.
  */
  virtual result<io_operation_state *> init_io_operation(span<byte> storage, io_handle *_h, io_operation_state_visitor *_visitor, registered_buffer_type &&b, deadline d, io_request<const_buffers_type> reqs, barrier_kind kind) noexcept = 0;

  //! Flushes any previously initiated i/o, if necessary for this i/o multiplexer
  LLFIO_HEADERS_ONLY_VIRTUAL_SPEC result<void> flush_inited_io_operations() noexcept { return success(); }

  //! Asks the system for the current state of the i/o, returning its current state.
  LLFIO_HEADERS_ONLY_VIRTUAL_SPEC io_operation_state_type check_io_operation(io_operation_state *op) noexcept { return op->current_state(); }

  //! Cancel an initiated i/o, returning its current state if successful.
  virtual result<io_operation_state_type> cancel_io_operation(io_operation_state *op, deadline d = {}) noexcept = 0;

  //! Statistics about the just returned `wait_for_completed_io()` operation
  struct wait_for_completed_io_statistics
  {
    size_t initiated_ios_completed{0};  //!< The number of initiated i/o which were completed by this call
  };

  /*! \brief Checks all i/o initiated on this i/o multiplexer to see which
  have completed, trying without guarantee to complete no more than `max_completions`
  completions. Can optionally sleep the thread until at least one initiated i/o completes,
  though may return zero completed i/o's if another thread used `.wake_check_for_any_completed_io()`.
  */
  virtual result<wait_for_completed_io_statistics> check_for_any_completed_io(deadline d = std::chrono::seconds(0), size_t max_completions = (size_t) -1) noexcept = 0;

  /*! \brief Can be called from any thread to wake any other single thread
  currently blocked within `check_for_any_completed_io()`. Which thread is
  woken is not specified.
  */
  virtual result<void> wake_check_for_any_completed_io() noexcept = 0;
};
//! A unique ptr to an i/o multiplexer implementation.
using io_multiplexer_ptr = std::unique_ptr<io_multiplexer>;

#if LLFIO_ENABLE_TEST_IO_MULTIPLEXERS
//! Namespace containing functions useful for test code
namespace test
{
#if defined(__linux__) || DOXYGEN_IS_IN_THE_HOUSE
// LLFIO_HEADERS_ONLY_FUNC_SPEC result<io_multiplexer_ptr> multiplexer_linux_epoll(size_t threads) noexcept;
// LLFIO_HEADERS_ONLY_FUNC_SPEC result<io_multiplexer_ptr> multiplexer_linux_io_uring() noexcept;
#endif
#if(defined(__FreeBSD__) || defined(__APPLE__)) || DOXYGEN_IS_IN_THE_HOUSE
// LLFIO_HEADERS_ONLY_FUNC_SPEC result<io_multiplexer_ptr> multiplexer_bsd_kqueue(size_t threads) noexcept;
#endif
#if defined(_WIN32) || DOXYGEN_IS_IN_THE_HOUSE
  /*! \brief Return a test i/o multiplexer implemented using Microsoft Windows IOCP.

  The multiplexer returned by this function is only a partial implementation, used
  only by the test suite. In particular it does not fully implement deadlined i/o.
  */
  LLFIO_HEADERS_ONLY_FUNC_SPEC result<io_multiplexer_ptr> multiplexer_win_iocp(size_t threads, bool disable_immediate_completions) noexcept;
#endif
}  // namespace test
#endif

//! \brief Thread local settings
namespace this_thread
{
  //! \brief Return the calling thread's current i/o multiplexer.
  LLFIO_HEADERS_ONLY_FUNC_SPEC io_multiplexer *multiplexer() noexcept;
  //! \brief Set the calling thread's current i/o multiplexer.
  LLFIO_HEADERS_ONLY_FUNC_SPEC void set_multiplexer(io_multiplexer *ctx) noexcept;
}  // namespace this_thread

// BEGIN make_free_functions.py
// END make_free_functions.py

LLFIO_V2_NAMESPACE_END

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#if LLFIO_HEADERS_ONLY == 1 && !defined(DOXYGEN_SHOULD_SKIP_THIS)
#define LLFIO_INCLUDED_BY_HEADER 1
#include "detail/impl/io_multiplexer.ipp"
#undef LLFIO_INCLUDED_BY_HEADER
#endif

#endif