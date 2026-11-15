/*****************************************************************/ /**
 * @file   thread.cpp
 * @brief  Platform-independent thread code: the thread body, join with a
 * deadline (a wait on thread_context::state), detach, tracking.
 *********************************************************************/
#include "internal.hpp"

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      constexpr const char* THREAD_HANDLE_TYPE = "thread";

      constexpr wait_word_t word(thread_state s) noexcept
      {
        return static_cast<wait_word_t>(s);
      }
    } // namespace

    namespace detail
    {
      void run_thread(thread_context* ctx) noexcept
      {
        ctx->id = current_thread_id();
        ctx->state.store(
            word(thread_state::THREAD_STATE_RUNNING), std::memory_order_release);
        wake_all(ctx->state);

        ctx->entry(ctx->user);

        // Read the hook first: without one, the owner may free the context
        // (detached thread) as soon as it reads EXITED
        void (*const on_exit)(thread_context*) noexcept = ctx->on_exit;
        // After this store, the context may be gone (no hook): the wake only
        // uses the address, as an identifier, and never touches the memory.
        std::atomic<wait_word_t>& state = ctx->state;
        state.store(
            word(thread_state::THREAD_STATE_EXITED), std::memory_order_release);
        wake_all(state);
        // The very last use of the context: the hook may free it
        if (on_exit != nullptr)
          on_exit(ctx);
      }
    } // namespace detail

    status<thread_create_result> thread_create(
        thread_context& ctx, const thread_desc& desc,
        std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(
          ctx.entry != nullptr, "osprim::thread_create: no entry function");
      ctx.state.store(
          word(thread_state::THREAD_STATE_STARTING), std::memory_order_relaxed);
      ctx.id     = 0;
      ctx.native = 0;
      const status<thread_create_result> s =
          detail::platform_thread_create(ctx, desc.stack_size);
      if (s.ok())
        detail::track_open(THREAD_HANDLE_TYPE, ctx.native, loc);
      return s;
    }

    thread_join_result thread_join(
        thread_context& ctx, awake_instant deadline,
        std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(
          ctx.native != 0, "osprim::thread_join: not created, or already released");
      wait_word_t v = ctx.state.load(std::memory_order_acquire);
      while (v != word(thread_state::THREAD_STATE_EXITED))
      {
        OSPRIM_ASSERT(
            v != word(thread_state::THREAD_STATE_RUNNING)
                || ctx.id != current_thread_id(),
            "osprim::thread_join: a thread cannot join itself");
        if (wait(ctx.state, v, deadline) == wait_result::WAIT_TIMED_OUT)
        {
          if (ctx.state.load(std::memory_order_acquire)
              != word(thread_state::THREAD_STATE_EXITED))
            return thread_join_result::THREAD_JOIN_TIMED_OUT;
          break;
        }
        v = ctx.state.load(std::memory_order_acquire);
      }
      // Forget the value while it is still held: once released, the OS may
      // give it to a new thread, which would be tracked under the same key
      const uint64_t native = ctx.native;
      detail::track_close(THREAD_HANDLE_TYPE, native, loc);
      ctx.native = 0;
      detail::platform_thread_join(native);
      return thread_join_result::THREAD_JOIN_OK;
    }

    void thread_detach(thread_context& ctx, std::source_location loc) noexcept
    {
      OSPRIM_ASSERT(
          ctx.native != 0,
          "osprim::thread_detach: not created, or already released");
      // Read and clear before releasing: the thread may exit, and its owner
      // free the context, as soon as it is detached
      const uint64_t native = ctx.native;
      ctx.native            = 0;
      detail::track_close(THREAD_HANDLE_TYPE, native, loc);
      detail::platform_thread_detach(native);
    }
  } // namespace v0_1
} // namespace osprim