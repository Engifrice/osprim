/*****************************************************************/ /**
 * @file   async_io.cpp
 * @brief  Async I/O: argument checks, tracking and its Debug checks. The
 * backends live in async_io_windows.cpp (I/O completion ports) and
 * async_io_linux.cpp (io_uring). Android and Apple have none: creating a
 * queue returns UNSUPPORTED without a system call.
 *
 * Tracking:
 * - A queue is tracked as ASYNC_IO_QUEUE_TRACK_TYPE under a process-unique
 *   id (next_handle_id()), kept in the backend state: never destroying a
 *   queue is a leak.
 * - Attaching writes that id into the file's tracking metadata
 *   (FILE_META_QUEUE): attaching twice, and submitting on a file attached
 *   to another queue (or none), are caught on every platform. The word
 *   stays after the queue is destroyed, as the attachment does on Windows.
 *********************************************************************/
#include "internal.hpp"

namespace osprim
{
  inline namespace v0_1
  {
#if OSPRIM_OS_WINDOWS || OSPRIM_OS_LINUX

  #if OSPRIM_TRACKING
    namespace
    {
      /// @brief Checks that `file` is attached to the queue `id`.
      void check_attached(file_handle file, uint64_t id) noexcept
      {
        detail::track_meta m{};
        if (!detail::track_get(
                detail::FILE_TRACK_TYPE, detail::file_key(file.native), &m))
          return;
        const uint64_t attached = m.words[detail::FILE_META_QUEUE];
        (void)attached; // only read by OSPRIM_ASSERT, which may be compiled out
        (void)id;
        OSPRIM_ASSERT(
            attached == id,
            "osprim::async_io_queue_submit: file not attached to this queue");
      }
    } // namespace
  #endif

    status<async_io_queue_create_result> async_io_queue_create(
        const async_io_queue_desc& desc, async_io_queue& out,
        std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(
          desc.capacity >= 1 && desc.capacity <= ASYNC_IO_CAPACITY_MAX,
          "osprim::async_io_queue_create: capacity out of range");
      const uint64_t id = detail::next_handle_id();
      const status<async_io_queue_create_result> s =
          detail::platform_async_io_create(desc.capacity, id, out);
      if (s.ok())
        detail::track_open(detail::ASYNC_IO_QUEUE_TRACK_TYPE, id, loc);
      return s;
    }

    void async_io_queue_destroy(async_io_queue& q, std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(
          detail::platform_async_io_in_flight(q) == 0,
          "osprim::async_io_queue_destroy: operations still in flight");
      detail::track_close(
          detail::ASYNC_IO_QUEUE_TRACK_TYPE, detail::platform_async_io_id(q), loc);
      detail::platform_async_io_destroy(q);
    }

    status<async_io_attach_result> async_io_queue_attach(
        async_io_queue& q, file_handle file, std::source_location loc) noexcept
    {
      (void)loc;
  #if OSPRIM_TRACKING
      detail::track_meta m{};
      const bool tracked = detail::track_get(
          detail::FILE_TRACK_TYPE, detail::file_key(file.native), &m);
      OSPRIM_ASSERT(
          !tracked || m.words[detail::FILE_META_QUEUE] == 0,
          "osprim::async_io_queue_attach: file already attached to a queue");
  #endif
      const status<async_io_attach_result> s =
          detail::platform_async_io_attach(q, file.native);
  #if OSPRIM_TRACKING
      if (s.ok() && tracked)
      {
        m.words[detail::FILE_META_QUEUE] = detail::platform_async_io_id(q);
        detail::track_set_meta(
            detail::FILE_TRACK_TYPE, detail::file_key(file.native), m);
      }
  #endif
      return s;
    }

    status<async_io_submit_result> async_io_queue_submit(
        async_io_queue& q, async_io_op* const* ops, size_t count, size_t& submitted,
        std::source_location loc) noexcept
    {
      (void)loc;
      OSPRIM_ASSERT(
          ops != nullptr || count == 0, "osprim::async_io_queue_submit: null ops");
  #if OSPRIM_TRACKING
      const uint64_t id = detail::platform_async_io_id(q);
  #endif
      for (size_t i = 0; i < count; ++i)
      {
        const async_io_op* op = ops[i];
        (void)
            op; // only read by OSPRIM_ASSERT and tracking, which may be compiled out
        OSPRIM_ASSERT(op != nullptr, "osprim::async_io_queue_submit: null op");
        OSPRIM_ASSERT(
            op->size <= FILE_IO_MAX,
            "osprim::async_io_queue_submit: size above FILE_IO_MAX");
        OSPRIM_ASSERT(
            op->offset <= static_cast<uint64_t>(INT64_MAX) - op->size,
            "osprim::async_io_queue_submit: offset + size above INT64_MAX");
        OSPRIM_ASSERT(
            op->buffer != nullptr || op->size == 0,
            "osprim::async_io_queue_submit: null buffer");
        OSPRIM_ASSERT(
            op->kind == async_io_op_kind::ASYNC_IO_OP_READ
                || op->kind == async_io_op_kind::ASYNC_IO_OP_WRITE,
            "osprim::async_io_queue_submit: invalid kind");
  #if OSPRIM_TRACKING
        check_attached(op->file, id);
        detail::file_check_io(
            op->file, op->offset, op->buffer, op->size,
            op->kind == async_io_op_kind::ASYNC_IO_OP_WRITE);
  #endif
      }
      submitted = 0;
      return detail::platform_async_io_submit(q, ops, count, &submitted);
    }

    status<async_io_wait_result> async_io_queue_wait(
        async_io_queue& q, async_io_op** out, size_t capacity, size_t min_count,
        awake_instant deadline, size_t& count) noexcept
    {
      OSPRIM_ASSERT(
          min_count <= capacity,
          "osprim::async_io_queue_wait: min_count above capacity");
      OSPRIM_ASSERT(
          out != nullptr || capacity == 0, "osprim::async_io_queue_wait: null out");
      count = 0;
      return detail::platform_async_io_wait(
          q, out, capacity, min_count, deadline, &count);
    }

    void async_io_queue_cancel(async_io_queue& q, async_io_op& op) noexcept
    {
      detail::platform_async_io_cancel(q, op);
    }

    void async_io_queue_wake(async_io_queue& q) noexcept
    {
      detail::platform_async_io_wake(q);
    }

#else // No backend: no queue can exist

    status<async_io_queue_create_result> async_io_queue_create(
        const async_io_queue_desc& desc, async_io_queue& out,
        std::source_location loc) noexcept
    {
      (void)desc;
      (void)out;
      (void)loc;
      return {
          async_io_queue_create_result::ASYNC_IO_QUEUE_CREATE_UNSUPPORTED,
          native_error_kind::NATIVE_ERROR_NONE, 0};
    }

    void async_io_queue_destroy(async_io_queue& q, std::source_location loc) noexcept
    {
      (void)q;
      (void)loc;
      OSPRIM_ASSERT(false, "osprim::async_io_queue_destroy: no queue can exist");
    }

    status<async_io_attach_result> async_io_queue_attach(
        async_io_queue& q, file_handle file, std::source_location loc) noexcept
    {
      (void)q;
      (void)file;
      (void)loc;
      OSPRIM_ASSERT(false, "osprim::async_io_queue_attach: no queue can exist");
      return {
          async_io_attach_result::ASYNC_IO_ATTACH_SYSTEM_ERROR,
          native_error_kind::NATIVE_ERROR_NONE, 0};
    }

    status<async_io_submit_result> async_io_queue_submit(
        async_io_queue& q, async_io_op* const* ops, size_t count, size_t& submitted,
        std::source_location loc) noexcept
    {
      (void)q;
      (void)ops;
      (void)count;
      (void)loc;
      OSPRIM_ASSERT(false, "osprim::async_io_queue_submit: no queue can exist");
      submitted = 0;
      return {
          async_io_submit_result::ASYNC_IO_SUBMIT_SYSTEM_ERROR,
          native_error_kind::NATIVE_ERROR_NONE, 0};
    }

    status<async_io_wait_result> async_io_queue_wait(
        async_io_queue& q, async_io_op** out, size_t capacity, size_t min_count,
        awake_instant deadline, size_t& count) noexcept
    {
      (void)q;
      (void)out;
      (void)capacity;
      (void)min_count;
      (void)deadline;
      OSPRIM_ASSERT(false, "osprim::async_io_queue_wait: no queue can exist");
      count = 0;
      return {
          async_io_wait_result::ASYNC_IO_WAIT_SYSTEM_ERROR,
          native_error_kind::NATIVE_ERROR_NONE, 0};
    }

    void async_io_queue_cancel(async_io_queue& q, async_io_op& op) noexcept
    {
      (void)q;
      (void)op;
      OSPRIM_ASSERT(false, "osprim::async_io_queue_cancel: no queue can exist");
    }

    void async_io_queue_wake(async_io_queue& q) noexcept
    {
      (void)q;
      OSPRIM_ASSERT(false, "osprim::async_io_queue_wake: no queue can exist");
    }

#endif
  } // namespace v0_1
} // namespace osprim
