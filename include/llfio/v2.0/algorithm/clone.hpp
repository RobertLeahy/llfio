/* A filesystem algorithm which clones a directory tree
(C) 2020 Niall Douglas <http://www.nedproductions.biz/> (12 commits)
File Created: May 2020


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

#ifndef LLFIO_ALGORITHM_CLONE_HPP
#define LLFIO_ALGORITHM_CLONE_HPP

#include "../file_handle.hpp"
#include "traverse.hpp"

//! \file clone.hpp Provides a directory tree clone algorithm.

LLFIO_V2_NAMESPACE_BEGIN

namespace algorithm
{
  /*! \brief Clone or copy the extents of the filesystem entity identified by `src`
  to `destdir` optionally renamed to `destleaf`.

  \return The number of bytes cloned or copied.
  \param src The file to clone or copy.
  \param destdir The base to lookup `destleaf` within.
  \param destleaf The leafname to use. If empty, use the same leafname as `src` currently has.
  \param preserve_timestamps Use `stat_t::stamp()` to preserve as much metadata from
  the original to the clone/copy as possible.
  \param force_copy_now Parameter to pass to `file_handle::clone_extents()` to force
  extents to be copied now, not copy-on-write lazily later.
  \param creation How to create the destination file handle.
  \param d Deadline by which to complete the operation.

  Firstly, a `file_handle` is constructed at the destination using `creation`,
  which defaults to always creating a new inode. The caching used for the
  destination handle is replicated from the source handle -- be aware that
  not caching metadata is expensive.

  Next `file_handle::clone_extents()` with `emulate_if_unsupported = false` is
  called on the whole file content. If extent cloning is supported, this will
  be very fast and not consume new disk space (note: except on networked filesystems).
  If the source file is sparsely allocated, the destination will have identical
  sparse allocation.

  If the previous operation did not succeed, the disk free space is checked
  using `statfs_t`, and if the copy would exceed current disk free space, the
  destination file is unlinked and an error code comparing equal to
  `errc::no_space_on_device` is returned.

  Next, `file_handle::clone_extents()` with `emulate_if_unsupported = true` is
  called on the whole file content. This copies only the allocated extents in
  blocks sized whatever is the large page size on this platform (2Mb on x64).

  Finally, if `preserve_timestamps` is true, the destination file handle is
  restamped with the metadata from the source file handle just before the
  destination file handle is closed.
  */
  LLFIO_HEADERS_ONLY_FUNC_SPEC result<file_handle::extent_type> clone_or_copy(const file_handle &src, const path_handle &destdir, path_view destleaf = {},
                                                                              bool preserve_timestamps = true, bool force_copy_now = false,
                                                                              file_handle::creation creation = file_handle::creation::always_new,
                                                                              deadline d = {}) noexcept;

#if 0
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4275)  // dll interface
#endif
  /*! \brief A visitor for the filesystem traversal and cloning algorithm.

  Note that at any time, returning a failure causes `clone()` to exit as soon
  as possible with the same failure.

  You can override the members here inherited from `traverse_visitor`, however note
  that `clone()` is entirely implemented using `traverse()`, so not calling the
  implementations here will affect operation.
  */
  struct LLFIO_DECL clone_copy_link_visitor : public traverse_visitor
  {
    std::chrono::steady_clock::duration timeout{std::chrono::seconds(10)};
    std::chrono::steady_clock::time_point begin;

    //! Constructs an instance with the default timeout of ten seconds.
    constexpr clone_copy_link_visitor() {}
    //! Constructs an instance with the specified timeout.
    constexpr explicit clone_copy_link_visitor(std::chrono::steady_clock::duration _timeout)
        : timeout(_timeout)
    {
    }

    //! This override ignores failures to traverse into the directory, and tries renaming the item into the base directory.
    virtual result<directory_handle> directory_open_failed(void *data, result<void>::error_type &&error, const directory_handle &dirh, path_view leaf, size_t depth) noexcept override;
    //! This override invokes deletion of all non-directory items. If there are no directory items, also deletes the directory.
    virtual result<void> post_enumeration(void *data, const directory_handle &dirh, directory_handle::buffers_type &contents, size_t depth) noexcept override;

    /*! \brief Called when the unlink of a file entry failed. The default
    implementation attempts to rename the entry into the base directory.
    If your reimplementation achieves the unlink, return true.

    \note May be called from multiple kernel threads concurrently.
    */
    virtual result<bool> unlink_failed(void *data, result<void>::error_type &&error, const directory_handle &dirh, directory_entry &entry, size_t depth) noexcept;
    /*! \brief Called when the rename of a file entry into the base directory
    failed. The default implementation ignores the failure. If your
    reimplementation achieves the rename, return true.

    \note May be called from multiple kernel threads concurrently.
    */
    virtual result<bool> rename_failed(void *data, result<void>::error_type &&error, const directory_handle &dirh, directory_entry &entry, size_t depth) noexcept
    {
      (void) data;
      (void) error;
      (void) dirh;
      (void) entry;
      (void) depth;
      return false;
    }
    /*! \brief Called when we have performed a single full round of reduction.

    \note Always called from the original kernel thread.
    */
    virtual result<void> reduction_round(void *data, size_t round_completed, size_t items_unlinked, size_t items_remaining) noexcept
    {
      (void) data;
      (void) round_completed;
      (void) items_unlinked;
      if(items_remaining > 0)
      {
        if(begin == std::chrono::steady_clock::time_point())
        {
          begin = std::chrono::steady_clock::now();
        }
        else if((std::chrono::steady_clock::now() - begin) > timeout)
        {
          return errc::timed_out;
        }
      }
      return success();
    }
  };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

  /*! \brief Copy the directory hierarchy identified by `srcdir` to `destdir`.

  This algorithm firstly traverses the source directory tree to calculate the filesystem
  blocks which would be used by a copy of all the directories. If insufficient disc space
  remains on the destination volume, the operation exits with an error code comparing equal to
  `errc::no_space_on_device`.

  If there is sufficient disc space, the directory hierarchy -- without any files -- is
  replicated exactly. Timestamps are NOT replicated (any subsequent newly added files
  would change the timestamps in any case).

  You should review the documentation for `algorithm::traverse()`, as this algorithm is
  entirely implemented using that algorithm.
  */
  LLFIO_HEADERS_ONLY_FUNC_SPEC result<size_t> copy_hierarchy(const directory_handle &srcdir, directory_handle &destdir, clone_copy_link_visitor *visitor = nullptr, size_t threads = 0, bool force_slow_path = false) noexcept;

  /*! \brief Clone or copy the extents of the filesystem entity identified by `srcdir`
  and `srcleaf`, and everything therein, to `destdir` named `srcleaf` or `destleaf`.

  \return The number of items cloned or copied.
  \param srcdir The base to lookup `srcleaf` within.
  \param srcleaf The leafname to lookup. If empty, treat `srcdir` as the directory to clone.
  \param destdir The base to lookup `destleaf` within.
  \param destleaf The leafname to lookup. If empty, treat `destdir` as the directory to clone.
  \param visitor The visitor to use.
  \param threads The number of kernel threads for `traverse()` to use.
  \param force_slow_path The parameter to pass to `traverse()`.

  - `srcleaf` empty and `destleaf` empty: Clone contents of `srcdir` into `destdir`.
  - `srcleaf` non-empty and `destleaf` empty: Clone `srcdir`/`srcleaf` into `destdir`/`srcleaf`.
  - `srcleaf` non-empty and `destleaf` non-empty: Clone `srcdir`/`srcleaf` into `destdir`/`destleaf`.

  This algorithm firstly traverses the source directory tree to calculate the filesystem
  blocks which could be used by a copy of the entity. If insufficient disc space
  remains on the destination volume for a copy of just the directories, the operation
  exits with an error code comparing equal to `errc::no_space_on_device`.

  A single large file is then chosen from the source, and an attempt is made to `file_handle::clone_extents()`
  with `emulate_if_unsupported = false` it into the destination. If extents cloning is
  not successful, and if the allocated blocks for both the files and the directories
  would exceed the free disc space on the destination volume, the destination is removed
  and an error code is returned comparing equal to `errc::no_space_on_device`.

  Otherwise, for every file in the source, its contents are cloned with `emulate_if_unsupported = true`
  into an equivalent file in the destination. This means that the contents are either
  cloned or copied to the best extent of your filesystems and kernel.

  You should review the documentation for `algorithm::traverse()`, as this algorithm is
  entirely implemented using that algorithm.
  */
  LLFIO_HEADERS_ONLY_FUNC_SPEC result<size_t> clone_or_copy(const path_handle &srcdir, path_view srcleaf, const path_handle &destdir, path_view destleaf = {}, clone_copy_link_visitor *visitor = nullptr, size_t threads = 0, bool force_slow_path = false) noexcept;
#endif
}  // namespace algorithm

LLFIO_V2_NAMESPACE_END

#if LLFIO_HEADERS_ONLY == 1 && !defined(DOXYGEN_SHOULD_SKIP_THIS)
#define LLFIO_INCLUDED_BY_HEADER 1
#include "../detail/impl/clone.ipp"
#undef LLFIO_INCLUDED_BY_HEADER
#endif


#endif
