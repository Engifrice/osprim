/*****************************************************************/ /**
 * @file   region.hpp
 * @brief  Mapped regions: a range of a file seen as memory.
 *
 * When to map, when to read:
 * - Map: random access into large read-only data on local storage, where
 *   the OS cache is the right cache.
 * - Read (file_read_at): streaming, removable or network storage, files
 *   other programs may change, and anything written that must survive a
 *   full disk. A mapped region turns every failure into a crash: a memory
 *   access has no error return (POSIX: SIGBUS; Windows:
 *   EXCEPTION_IN_PAGE_ERROR). Device errors, a file truncated by another
 *   program, and a full disk while writing into unreserved space all
 *   crash the process.
 *
 * Rules:
 * - A region is {data, size} and nothing more. Any offset is accepted:
 *   the library maps from the offset rounded down to region_granularity()
 *   and `data` points at the requested byte. Only [data, data + size)
 *   belongs to the region: neighbouring bytes may be mapped file data, and
 *   touching them is undefined.
 * - The range must lie inside the file when mapping (no implicit growth).
 * - A region does not need the file handle afterwards: the handle may be
 *   closed at once (but durable writes need it, see region_flush()).
 * - Visibility: writes through READ_WRITE regions are seen at once by
 *   every region of the same file, in any process (local files).
 *   Regions and file_read_at() / file_write_at() on the same range are not
 *   guaranteed to agree (Windows promises nothing): region_flush() in
 *   between. Unbuffered handles never agree with regions.
 *********************************************************************/
#ifndef OSPRIM_REGION_HPP
#define OSPRIM_REGION_HPP

#include <osprim/base.hpp>
#include <osprim/file.hpp>

#include <stddef.h>
#include <stdint.h>

#include <source_location>

namespace osprim
{
  inline namespace v0_1
  {
    /// @brief What a region allows.
    ///
    /// | access        | file needs       | writes reach the file | Windows commit charge |
    /// |---------------|------------------|-----------------------|-----------------------|
    /// | READ          | read             | -                     | none                  |
    /// | READ_WRITE    | read and write   | yes                   | none                  |
    /// | COPY_ON_WRITE | read             | never                 | the whole region, at  |
    /// |               |                  |                       | map time              |
    ///
    /// COPY_ON_WRITE: whether later changes to the file show in pages not
    /// yet written is unspecified.
    enum class region_access : uint8_t
    {
      REGION_ACCESS_READ,
      REGION_ACCESS_READ_WRITE,
      REGION_ACCESS_COPY_ON_WRITE,
    };

    /// @brief A mapped range of a file.
    struct region
    {
      void* data;  ///< The byte at the requested offset
      size_t size; ///< As requested
    };

    /// @brief Alignment the OS maps at, in bytes (a power of two): the page
    /// size on POSIX (4 or 16 KiB), the allocation granularity on Windows
    /// (64 KiB). Offsets that are multiples of it waste no address space.
    OSPRIM_API size_t region_granularity() noexcept;

    /// @brief Result of region_map().
    enum class region_map_result : uint8_t
    {
      REGION_MAP_OK,
      REGION_MAP_EMPTY,         ///< `size` is 0 (nothing mapped)
      REGION_MAP_OUT_OF_RANGE,  ///< `offset` + `size` beyond the file's current size
      REGION_MAP_ACCESS_DENIED, ///< The file's access does not allow `access`
      REGION_MAP_OUT_OF_MEMORY, ///< No address space, or (COPY_ON_WRITE) no commit
      REGION_MAP_UNSUPPORTED,   ///< The filesystem cannot be mapped
      REGION_MAP_SYSTEM_ERROR,  ///< Any other refusal (see native_code)
    };

    /// @brief Maps `size` bytes of `file` from `offset`. Tracked builds
    /// report READ_WRITE regions over unreserved space (holes) with
    /// DIAGNOSTIC_REGION_OVER_HOLE.
    /// @pre `file` open (and not unbuffered while being written)
    OSPRIM_API status<region_map_result> region_map(
        file_handle file, uint64_t offset, size_t size, region_access access,
        region& out,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Unmaps the region. Unflushed writes still reach the file later.
    /// @pre `r` is exactly as region_map() returned it, and no other thread
    /// touches it
    OSPRIM_API void region_unmap(
        region r,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Result of region_flush().
    enum class region_flush_result : uint8_t
    {
      REGION_FLUSH_OK,
      REGION_FLUSH_IO_ERROR,     ///< The device failed
      REGION_FLUSH_SYSTEM_ERROR, ///< Any other refusal (see native_code)
    };

    /// @brief Writes the dirty pages of [r.data + offset, + size) to the
    /// file and waits (msync MS_SYNC, FlushViewOfFile). This reaches the
    /// OS, not stable storage: call file_flush() on a handle to the file
    /// afterwards for durability.
    /// @pre READ_WRITE region; `offset` + `size` <= `r.size`
    OSPRIM_API status<region_flush_result> region_flush(
        region r, size_t offset, size_t size) noexcept;
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_REGION_HPP
