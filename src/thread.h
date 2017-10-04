#ifndef THREAD_H
#define THREAD_H

#include <atomic>
#include <string>
#include <memory>
#include <utility>
#include <functional>
#include <vector>
#include <iostream>
#include <sstream>
#include <uv.h>

#include "errable.h"
#include "result.h"
#include "queue.h"
#include "message.h"
#include "status.h"
#include "thread_starter.h"

// Abstract superclass used by the Hub to manage and communicate with separate threads of execution.
//
// For the most part, public methods are intended to be executed from the main thread. Protected and private methods
// are meant to be called from the thread itself.
class Thread : public SyncErrable {
public:
  // Construct a stopped Thread.
  //
  // * `name` is used to mark status errors by the `Errable` superclass. It's accessible by `Thread::get_source()`.
  // * `main_callback` is used to trigger an async handle on the libuv event loop to consume any waiting messages
  //   on this thread's out queue via `Thread::receive_all()`.
  // * If provided, `starter` allows subclasses to customize configuration that can be manipulated offline (while the
  //   thread is stopped). See `ThreadStarter` for details.
  Thread(
    std::string name,
    uv_async_t *main_callback,
    std::unique_ptr<ThreadStarter> starter = std::unique_ptr<ThreadStarter>(new ThreadStarter())
  );

  // Start the thread.
  //
  // The thread will be `STARTING` immediately, but may take some time to actually begin execution. If the thread
  // fails to start, its error state will be set and returned.
  Result<> run();

  // Enqueue a `Message` on this thread's input queue and schedule a wake-up event to consume it. Returns `true` if
  // an offline Ack message was created by this call. The caller should immediately call `Thread::receive_all()`
  // to consume it, because the uv_async_t callback will *not* be triggered.
  Result<bool> send(Message &&message);

  // Enqueue an entire collection of `Messages` on this thread's input queue and schedule a thread wake-up to consume
  // them. The input queue and wake-up notification are only triggered once, so this method is much more efficient
  // than calling `Thread::send()` in a loop.
  //
  // Returns `true` if at least one offline Ack message was created by this call. The caller should immediately call
  // `Thread::receive_all()` to consume them, because the uv_async_t callback will *not* be triggered.
  template <class InputIt>
  Result<bool> send_all(InputIt begin, InputIt end);

  // Accept any and all `Messages` that have been emitted by this thread since the last `Thread::receive_all()` call.
  // The output queue is emptied after this call returns. If no `Messages` are waiting, a null pointer is returned
  // instead.
  Result< std::unique_ptr<std::vector<Message>> > receive_all();

  // Re-send any `Messages` that were sent between the acceptance of the message batch that caused the thread to
  // stop and the transition of the thread to the `STOPPING` phase. Note that this may cause the thread to immediately
  // run again.
  Result<bool> drain();

  // Override to populate the appropriate fields within a `Status` structure.
  virtual void collect_status(Status &status) = 0;

protected:
  // Invoked on the newly created thread. Responsible for performing thread startup, consuming any `ThreadStart`
  // initialization and transitioning to the `RUNNING` phase. Calls `Thread::body()` to perform subclass-defined
  // work, which subclasses should override with their main message loop. Transitions the thread to the `STOPPED`
  // phase just before existing.
  void start();

  // Override to perform the primary message loop of a subclass. Call `Thread::mark_stopping()` and to stop the thread
  // in an orderly fashion.
  virtual Result<> body() { return ok_result(); }

  // Override to hint that `Thread::body()` should wake from sleep and call `Thread::handle_commands()` to accept
  // `Messages` waiting on this thread's input queue.
  virtual Result<> wake() { return ok_result(); }

  // Enqueue a `Message` to be sent back to the main thread on the output queue. Trigger the `uv_async_t` callback to
  // prompt the main thread to consume it at its nearest convenience.
  Result<> emit(Message &&message);

  // Enqueue a batch of `Messages` to be sent back to the main thread on the output queue. Trigger the `uv_async_t`
  // callback to prompt the main thread to consume them at its nearest convenience.
  //
  // The output queue and async notification are only triggered once, so this method is much more efficient than
  // calling `Thread::emit()` in a loop. See `MessageBuffer` and `ChannelMessageBuffer` for mechanisms to collect
  // `Messages` into batches.
  template <class InputIt>
  Result<> emit_all(InputIt begin, InputIt end);

  // Possible follow-on actions to be taken as a result of a received `Command`.
  enum CommandOutcome {
    NOTHING, // No action. Use this when the ack will be delivered asynchronously.
    ACK, // Buffer an ack message corresponding to this Command to acknowledge receipt.
    TRIGGER_STOP, // Prompt the thread to begin shutting down after it finishes this message batch.
    PREVENT_STOP // Cancel the most recent TRIGGER_STOP received before this message within the batch.
  };

  // Possible follow-on actions to be taken as the result of a `CommandPayload` delivered to this thread while it's
  // `STOPPED`.
  enum OfflineCommandOutcome {
    OFFLINE_ACK, // Synchronously produce an ack for this `Command` and return `true` from the send method.
    TRIGGER_RUN // Enqueue this message and start the thread to consume it.
  };

  // Process any messages sent to this thread from the main thread. Return the number of messages processed.
  //
  // Dispatch to the appropriate `handle_xyz_command()` methods for specific commands.
  //
  // Subclasses should call this method once per cycle from their `Thread::run()` override, after being awaken
  // by a `Thread::wake()` call.
  Result<size_t> handle_commands();

  // Override to add a root directory.
  virtual Result<CommandOutcome> handle_add_command(const CommandPayload *payload);

  // Override to remove a root directory. Optionally, trigger a possible thread shutdown by returning `TRIGGER_STOP`.
  virtual Result<CommandOutcome> handle_remove_command(const CommandPayload *payload);

  // Configure this thread to log to a file.
  Result<CommandOutcome> handle_log_file_command(const CommandPayload *payload);

  // Configure this thread to log to stderr.
  Result<CommandOutcome> handle_log_stderr_command(const CommandPayload *payload);

  // Configure this thread to log to stdout.
  Result<CommandOutcome> handle_log_stdout_command(const CommandPayload *payload);

  // Disable logging from this thread.
  Result<CommandOutcome> handle_log_disable_command(const CommandPayload *payload);

  // Called when a `Message` with an unexpected command type is received. Logs the message and acknowledges.
  Result<CommandOutcome> handle_unknown_command(const CommandPayload *payload);

  // Override to determine how this thread responds to specific commands delivered while it is `STOPPED`.
  //
  // For a given command message, this should either:
  // * Alter the `ThreadStart` to remember this configuration option on the next start, and return `OFFLINE_ACK`
  //   to acknowledge synchronously; or
  // * Return `TRIGGER_RUN` to cause the thread to automatically start (and consume this message on startup).
  //
  // The base class implementation records logging configurations in its `ThreadStart` and ack's all other commands
  // without effect. Override and call the base to handle logging by default.
  virtual Result<OfflineCommandOutcome> handle_offline_command(const CommandPayload *payload);

  using CommandHandler = Result<CommandOutcome> (Thread::*)(const CommandPayload*);
  static const CommandHandler command_handlers[];

  // Atomically transition this thread to the specifed state.
  void mark_stopped() { state.store(State::STOPPED); }
  void mark_starting() { state.store(State::STARTING); }
  void mark_running() { state.store(State::RUNNING); }
  void mark_stopping() { state.store(State::STOPPING); }

  // Atomically check if this thread is in a specific state.
  bool is_starting() { return state.load() == State::STARTING; }
  bool is_running() { return state.load() == State::RUNNING; }
  bool is_stopped() { return state.load() == State::STOPPED; }
  bool is_stopping() { return state.load() == State::STOPPING; }

  // Return a string describing the thread's current state.
  std::string state_name();

  // Access queue statistics for `Thread::collect_status()`.
  std::string get_in_queue_error() { return in.get_error(); }
  size_t get_in_queue_size() { return in.size(); }
  std::string get_out_queue_error() { return out.get_error(); }
  size_t get_out_queue_size() { return out.size(); }

private:
  enum State {
    STOPPED,
    STARTING,
    RUNNING,
    STOPPING
  };

  std::atomic<State> state;

  std::unique_ptr<ThreadStarter> starter;

  Queue in;
  Queue out;

  uv_async_t *main_callback;

  uv_thread_t uv_handle;
  std::function<void()> work_fn;

  std::unique_ptr<std::vector<Message>> dead_letter_office;
};

template <class InputIt>
Result<bool> Thread::send_all(InputIt begin, InputIt end)
{
  if (!is_healthy()) return health_err_result<bool>();

  if (is_stopping()) {
    uv_thread_join(&uv_handle);

    if (dead_letter_office) {
      std::unique_ptr<std::vector<Message>> dead_letters = std::move(dead_letter_office);
      dead_letter_office.reset(nullptr);

      for (InputIt it = begin; it != end; ++it) {
        dead_letters->emplace_back(std::move(*it));
      }

      return send_all(dead_letters->begin(), dead_letters->end());
    }
  }

  if (is_stopped()) {
    bool should_run = false;
    std::vector<Message> acks;

    for (InputIt it = begin; it != end; ++it) {
      const CommandPayload *command = it->as_command();
      if (command == nullptr) {
        std::ostringstream m;
        m << "Non-command message " << *it << " sent";
        acks.emplace_back(Message::ack(*it, false, m.str()));
        continue;
      }

      Result<OfflineCommandOutcome> r0 = handle_offline_command(command);
      if (r0.is_error() || r0.get_value() == OFFLINE_ACK) {
        acks.emplace_back(Message::ack(*it, r0.propagate_as_void()));
      } else if (r0.get_value() == TRIGGER_RUN) {
        Result<> r1 = in.enqueue(std::move(*it));
        if (r1.is_error()) return r1.propagate<bool>();

        should_run = true;
      } else {
        std::ostringstream m;
        m << "Message " << *it << " returned invalid offline outcome " << r0;
        acks.emplace_back(Message::ack(*it, false, m.str()));
      }
    }

    if (!acks.empty()) {
      Result<> r2 = out.enqueue_all(acks.begin(), acks.end());
      if (r2.is_error()) return r2.propagate<bool>();
    }

    if (should_run) {
      return run().propagate(!acks.empty());
    }

    return ok_result(!acks.empty());
  }

  Result<> r3 = in.enqueue_all(begin, end);
  if (r3.is_error()) return r3.template propagate<bool>();

  if (is_running()) {
    return wake().propagate(false);
  }

  return ok_result(false);
}

template <class InputIt>
Result<> Thread::emit_all(InputIt begin, InputIt end)
{
  if (!is_healthy()) return health_err_result();

  Result<> qr = out.enqueue_all(begin, end);
  if (qr.is_error()) return qr;

  int uv_err = uv_async_send(main_callback);
  if (uv_err) {
    return error_result(uv_strerror(uv_err));
  }

  return ok_result();
}

std::ostream &operator<<(std::ostream &out, const Thread &th);

#endif
