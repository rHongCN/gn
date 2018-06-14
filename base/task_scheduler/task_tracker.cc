// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/task_scheduler/task_tracker.h"

#include <limits>
#include <string>
#include <vector>

#include "base/base_switches.h"
#include "base/callback.h"
#include "base/command_line.h"
#include "base/json/json_writer.h"
#include "base/memory/ptr_util.h"
#include "base/sequence_token.h"
#include "base/strings/string_util.h"
#include "base/synchronization/condition_variable.h"
#include "base/task_scheduler/scoped_set_task_priority_for_current_thread.h"
#include "base/threading/sequence_local_storage_map.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/threading/thread_restrictions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "base/values.h"

namespace base {
namespace internal {

namespace {

constexpr char kParallelExecutionMode[] = "parallel";
constexpr char kSequencedExecutionMode[] = "sequenced";
constexpr char kSingleThreadExecutionMode[] = "single thread";

// An immutable copy of a scheduler task's info required by tracing.
class TaskTracingInfo {
 public:
  TaskTracingInfo(const TaskTraits& task_traits,
                  const char* execution_mode,
                  const SequenceToken& sequence_token)
      : task_traits_(task_traits),
        execution_mode_(execution_mode),
        sequence_token_(sequence_token) {}

 private:
  const TaskTraits task_traits_;
  const char* const execution_mode_;
  const SequenceToken sequence_token_;

  DISALLOW_COPY_AND_ASSIGN(TaskTracingInfo);
};

// Returns the maximum number of TaskPriority::BACKGROUND sequences that can be
// scheduled concurrently based on command line flags.
int GetMaxNumScheduledBackgroundSequences() {
  // The CommandLine might not be initialized if TaskScheduler is initialized
  // in a dynamic library which doesn't have access to argc/argv.
  if (CommandLine::InitializedForCurrentProcess() &&
      CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kDisableBackgroundTasks)) {
    return 0;
  }
  return std::numeric_limits<int>::max();
}

}  // namespace

// Atomic internal state used by TaskTracker. Sequential consistency shouldn't
// be assumed from these calls (i.e. a thread reading
// |HasShutdownStarted() == true| isn't guaranteed to see all writes made before
// |StartShutdown()| on the thread that invoked it).
class TaskTracker::State {
 public:
  State() = default;

  // Sets a flag indicating that shutdown has started. Returns true if there are
  // tasks blocking shutdown. Can only be called once.
  bool StartShutdown() {
    const auto new_value =
        subtle::NoBarrier_AtomicIncrement(&bits_, kShutdownHasStartedMask);

    // Check that the "shutdown has started" bit isn't zero. This would happen
    // if it was incremented twice.
    DCHECK(new_value & kShutdownHasStartedMask);

    const auto num_tasks_blocking_shutdown =
        new_value >> kNumTasksBlockingShutdownBitOffset;
    return num_tasks_blocking_shutdown != 0;
  }

  // Returns true if shutdown has started.
  bool HasShutdownStarted() const {
    return subtle::NoBarrier_Load(&bits_) & kShutdownHasStartedMask;
  }

  // Returns true if there are tasks blocking shutdown.
  bool AreTasksBlockingShutdown() const {
    const auto num_tasks_blocking_shutdown =
        subtle::NoBarrier_Load(&bits_) >> kNumTasksBlockingShutdownBitOffset;
    DCHECK_GE(num_tasks_blocking_shutdown, 0);
    return num_tasks_blocking_shutdown != 0;
  }

  // Increments the number of tasks blocking shutdown. Returns true if shutdown
  // has started.
  bool IncrementNumTasksBlockingShutdown() {
#if DCHECK_IS_ON()
    // Verify that no overflow will occur.
    const auto num_tasks_blocking_shutdown =
        subtle::NoBarrier_Load(&bits_) >> kNumTasksBlockingShutdownBitOffset;
    DCHECK_LT(num_tasks_blocking_shutdown,
              std::numeric_limits<subtle::Atomic32>::max() -
                  kNumTasksBlockingShutdownIncrement);
#endif

    const auto new_bits = subtle::NoBarrier_AtomicIncrement(
        &bits_, kNumTasksBlockingShutdownIncrement);
    return new_bits & kShutdownHasStartedMask;
  }

  // Decrements the number of tasks blocking shutdown. Returns true if shutdown
  // has started and the number of tasks blocking shutdown becomes zero.
  bool DecrementNumTasksBlockingShutdown() {
    const auto new_bits = subtle::NoBarrier_AtomicIncrement(
        &bits_, -kNumTasksBlockingShutdownIncrement);
    const bool shutdown_has_started = new_bits & kShutdownHasStartedMask;
    const auto num_tasks_blocking_shutdown =
        new_bits >> kNumTasksBlockingShutdownBitOffset;
    DCHECK_GE(num_tasks_blocking_shutdown, 0);
    return shutdown_has_started && num_tasks_blocking_shutdown == 0;
  }

 private:
  static constexpr subtle::Atomic32 kShutdownHasStartedMask = 1;
  static constexpr subtle::Atomic32 kNumTasksBlockingShutdownBitOffset = 1;
  static constexpr subtle::Atomic32 kNumTasksBlockingShutdownIncrement =
      1 << kNumTasksBlockingShutdownBitOffset;

  // The LSB indicates whether shutdown has started. The other bits count the
  // number of tasks blocking shutdown.
  // No barriers are required to read/write |bits_| as this class is only used
  // as an atomic state checker, it doesn't provide sequential consistency
  // guarantees w.r.t. external state. Sequencing of the TaskTracker::State
  // operations themselves is guaranteed by the AtomicIncrement RMW (read-
  // modify-write) semantics however. For example, if two threads are racing to
  // call IncrementNumTasksBlockingShutdown() and StartShutdown() respectively,
  // either the first thread will win and the StartShutdown() call will see the
  // blocking task or the second thread will win and
  // IncrementNumTasksBlockingShutdown() will know that shutdown has started.
  subtle::Atomic32 bits_ = 0;

  DISALLOW_COPY_AND_ASSIGN(State);
};

struct TaskTracker::PreemptedBackgroundSequence {
  PreemptedBackgroundSequence() = default;
  PreemptedBackgroundSequence(scoped_refptr<Sequence> sequence_in,
                              TimeTicks next_task_sequenced_time_in,
                              CanScheduleSequenceObserver* observer_in)
      : sequence(std::move(sequence_in)),
        next_task_sequenced_time(next_task_sequenced_time_in),
        observer(observer_in) {}
  PreemptedBackgroundSequence(PreemptedBackgroundSequence&& other) = default;
  ~PreemptedBackgroundSequence() = default;
  PreemptedBackgroundSequence& operator=(PreemptedBackgroundSequence&& other) =
      default;
  bool operator<(const PreemptedBackgroundSequence& other) const {
    return next_task_sequenced_time < other.next_task_sequenced_time;
  }
  bool operator>(const PreemptedBackgroundSequence& other) const {
    return next_task_sequenced_time > other.next_task_sequenced_time;
  }

  // A background sequence waiting to be scheduled.
  scoped_refptr<Sequence> sequence;

  // The sequenced time of the next task in |sequence|.
  TimeTicks next_task_sequenced_time;

  // An observer to notify when |sequence| can be scheduled.
  CanScheduleSequenceObserver* observer = nullptr;

 private:
  DISALLOW_COPY_AND_ASSIGN(PreemptedBackgroundSequence);
};

TaskTracker::TaskTracker(StringPiece histogram_label)
    : TaskTracker(histogram_label, GetMaxNumScheduledBackgroundSequences()) {}

TaskTracker::TaskTracker(StringPiece histogram_label,
                         int max_num_scheduled_background_sequences)
    : state_(new State),
      flush_cv_(flush_lock_.CreateConditionVariable()),
      shutdown_lock_(&flush_lock_),
      max_num_scheduled_background_sequences_(
          max_num_scheduled_background_sequences),
      tracked_ref_factory_(this) {
}

TaskTracker::~TaskTracker() = default;

void TaskTracker::Shutdown() {
  PerformShutdown();
  DCHECK(IsShutdownComplete());

  // Unblock FlushForTesting() and perform the FlushAsyncForTesting callback
  // when shutdown completes.
  {
    AutoSchedulerLock auto_lock(flush_lock_);
    flush_cv_->Signal();
  }
  CallFlushCallbackForTesting();
}

void TaskTracker::FlushForTesting() {
  AutoSchedulerLock auto_lock(flush_lock_);
  while (subtle::Acquire_Load(&num_incomplete_undelayed_tasks_) != 0 &&
         !IsShutdownComplete()) {
    flush_cv_->Wait();
  }
}

void TaskTracker::FlushAsyncForTesting(OnceClosure flush_callback) {
  DCHECK(flush_callback);
  {
    AutoSchedulerLock auto_lock(flush_lock_);
    DCHECK(!flush_callback_for_testing_)
        << "Only one FlushAsyncForTesting() may be pending at any time.";
    flush_callback_for_testing_ = std::move(flush_callback);
  }

  if (subtle::Acquire_Load(&num_incomplete_undelayed_tasks_) == 0 ||
      IsShutdownComplete()) {
    CallFlushCallbackForTesting();
  }
}

bool TaskTracker::WillPostTask(const Task& task) {
  DCHECK(task.task);

  if (!BeforePostTask(task.traits.shutdown_behavior()))
    return false;

  if (task.delayed_run_time.is_null())
    subtle::NoBarrier_AtomicIncrement(&num_incomplete_undelayed_tasks_, 1);

  return true;
}

scoped_refptr<Sequence> TaskTracker::WillScheduleSequence(
    scoped_refptr<Sequence> sequence,
    CanScheduleSequenceObserver* observer) {
  const SequenceSortKey sort_key = sequence->GetSortKey();

  // A foreground sequence can always be scheduled.
  if (sort_key.priority() != TaskPriority::BACKGROUND)
    return sequence;

  // It is convenient not to have to specify an observer when scheduling
  // foreground sequences in tests.
  DCHECK(observer);

  AutoSchedulerLock auto_lock(background_lock_);

  if (num_scheduled_background_sequences_ <
      max_num_scheduled_background_sequences_) {
    ++num_scheduled_background_sequences_;
    return sequence;
  }

  preempted_background_sequences_.emplace(
      std::move(sequence), sort_key.next_task_sequenced_time(), observer);
  return nullptr;
}

scoped_refptr<Sequence> TaskTracker::RunAndPopNextTask(
    scoped_refptr<Sequence> sequence,
    CanScheduleSequenceObserver* observer) {
  DCHECK(sequence);

  // Run the next task in |sequence|.
  Optional<Task> task = sequence->TakeTask();
  // TODO(fdoray): Support TakeTask() returning null. https://crbug.com/783309
  DCHECK(task);

  const TaskShutdownBehavior shutdown_behavior =
      task->traits.shutdown_behavior();
  const TaskPriority task_priority = task->traits.priority();
  const bool can_run_task = BeforeRunTask(shutdown_behavior);
  const bool is_delayed = !task->delayed_run_time.is_null();

  RunOrSkipTask(std::move(task.value()), sequence.get(), can_run_task);
  if (can_run_task)
    AfterRunTask(shutdown_behavior);

  if (!is_delayed)
    DecrementNumIncompleteUndelayedTasks();

  const bool sequence_is_empty_after_pop = sequence->Pop();

  // Never reschedule a Sequence emptied by Pop(). The contract is such that
  // next poster to make it non-empty is responsible to schedule it.
  if (sequence_is_empty_after_pop)
    sequence = nullptr;

  if (task_priority == TaskPriority::BACKGROUND) {
    // Allow |sequence| to be rescheduled only if its next task is set to run
    // earlier than the earliest currently preempted sequence
    return ManageBackgroundSequencesAfterRunningTask(std::move(sequence),
                                                     observer);
  }

  return sequence;
}

bool TaskTracker::HasShutdownStarted() const {
  return state_->HasShutdownStarted();
}

bool TaskTracker::IsShutdownComplete() const {
  AutoSchedulerLock auto_lock(shutdown_lock_);
  return shutdown_event_ && shutdown_event_->IsSignaled();
}

void TaskTracker::SetHasShutdownStartedForTesting() {
  AutoSchedulerLock auto_lock(shutdown_lock_);

  // Create a dummy |shutdown_event_| to satisfy TaskTracker's expectation of
  // its existence during shutdown (e.g. in OnBlockingShutdownTasksComplete()).
  shutdown_event_.reset(
      new WaitableEvent(WaitableEvent::ResetPolicy::MANUAL,
                        WaitableEvent::InitialState::NOT_SIGNALED));

  state_->StartShutdown();
}

void TaskTracker::RunOrSkipTask(Task task,
                                Sequence* sequence,
                                bool can_run_task) {
  const bool previous_singleton_allowed =
      ThreadRestrictions::SetSingletonAllowed(
          task.traits.shutdown_behavior() !=
          TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN);
  const bool previous_io_allowed =
      ThreadRestrictions::SetIOAllowed(task.traits.may_block());
  const bool previous_wait_allowed = ThreadRestrictions::SetWaitAllowed(
      task.traits.with_base_sync_primitives());

  {
    const SequenceToken& sequence_token = sequence->token();
    DCHECK(sequence_token.IsValid());
    ScopedSetSequenceTokenForCurrentThread
        scoped_set_sequence_token_for_current_thread(sequence_token);
    ScopedSetTaskPriorityForCurrentThread
        scoped_set_task_priority_for_current_thread(task.traits.priority());
    ScopedSetSequenceLocalStorageMapForCurrentThread
        scoped_set_sequence_local_storage_map_for_current_thread(
            sequence->sequence_local_storage());

    // Set up TaskRunnerHandle as expected for the scope of the task.
    std::unique_ptr<SequencedTaskRunnerHandle> sequenced_task_runner_handle;
    std::unique_ptr<ThreadTaskRunnerHandle> single_thread_task_runner_handle;
    DCHECK(!task.sequenced_task_runner_ref ||
           !task.single_thread_task_runner_ref);
    if (task.sequenced_task_runner_ref) {
      sequenced_task_runner_handle.reset(
          new SequencedTaskRunnerHandle(task.sequenced_task_runner_ref));
    } else if (task.single_thread_task_runner_ref) {
      single_thread_task_runner_handle.reset(
          new ThreadTaskRunnerHandle(task.single_thread_task_runner_ref));
    }

    if (can_run_task) {
      std::move(task.task).Run();
    }

    // Make sure the arguments bound to the callback are deleted within the
    // scope in which the callback runs.
    task.task = OnceClosure();
  }

  ThreadRestrictions::SetWaitAllowed(previous_wait_allowed);
  ThreadRestrictions::SetIOAllowed(previous_io_allowed);
  ThreadRestrictions::SetSingletonAllowed(previous_singleton_allowed);
}

void TaskTracker::PerformShutdown() {
  {
    AutoSchedulerLock auto_lock(shutdown_lock_);

    // This method can only be called once.
    DCHECK(!shutdown_event_);
    DCHECK(!state_->HasShutdownStarted());

    shutdown_event_.reset(
        new WaitableEvent(WaitableEvent::ResetPolicy::MANUAL,
                          WaitableEvent::InitialState::NOT_SIGNALED));

    const bool tasks_are_blocking_shutdown = state_->StartShutdown();

    // From now, if a thread causes the number of tasks blocking shutdown to
    // become zero, it will call OnBlockingShutdownTasksComplete().

    if (!tasks_are_blocking_shutdown) {
      // If another thread posts a BLOCK_SHUTDOWN task at this moment, it will
      // block until this method releases |shutdown_lock_|. Then, it will fail
      // DCHECK(!shutdown_event_->IsSignaled()). This is the desired behavior
      // because posting a BLOCK_SHUTDOWN task when TaskTracker::Shutdown() has
      // started and no tasks are blocking shutdown isn't allowed.
      shutdown_event_->Signal();
      return;
    }
  }

  // Remove the cap on the maximum number of background sequences that can be
  // scheduled concurrently. Done after starting shutdown to ensure that non-
  // BLOCK_SHUTDOWN sequences don't get a chance to run and that BLOCK_SHUTDOWN
  // sequences run on threads running with a normal priority.
  SetMaxNumScheduledBackgroundSequences(std::numeric_limits<int>::max());

  // It is safe to access |shutdown_event_| without holding |lock_| because the
  // pointer never changes after being set above.
  {
    base::ThreadRestrictions::ScopedAllowWait allow_wait;
    shutdown_event_->Wait();
  }
}

void TaskTracker::SetMaxNumScheduledBackgroundSequences(
    int max_num_scheduled_background_sequences) {
  std::vector<PreemptedBackgroundSequence> sequences_to_schedule;

  {
    AutoSchedulerLock auto_lock(background_lock_);
    max_num_scheduled_background_sequences_ =
        max_num_scheduled_background_sequences;

    while (num_scheduled_background_sequences_ <
               max_num_scheduled_background_sequences &&
           !preempted_background_sequences_.empty()) {
      sequences_to_schedule.push_back(
          GetPreemptedBackgroundSequenceToScheduleLockRequired());
    }
  }

  for (auto& sequence_to_schedule : sequences_to_schedule)
    SchedulePreemptedBackgroundSequence(std::move(sequence_to_schedule));
}

TaskTracker::PreemptedBackgroundSequence
TaskTracker::GetPreemptedBackgroundSequenceToScheduleLockRequired() {
  background_lock_.AssertAcquired();
  DCHECK(!preempted_background_sequences_.empty());

  ++num_scheduled_background_sequences_;
  DCHECK_LE(num_scheduled_background_sequences_,
            max_num_scheduled_background_sequences_);

  // The const_cast on top is okay since the PreemptedBackgroundSequence is
  // transactionnaly being popped from |preempted_background_sequences_| right
  // after and the move doesn't alter the sort order (a requirement for the
  // Windows STL's consistency debug-checks for std::priority_queue::top()).
  PreemptedBackgroundSequence popped_sequence =
      std::move(const_cast<PreemptedBackgroundSequence&>(
          preempted_background_sequences_.top()));
  preempted_background_sequences_.pop();
  return popped_sequence;
}

void TaskTracker::SchedulePreemptedBackgroundSequence(
    PreemptedBackgroundSequence sequence_to_schedule) {
  DCHECK(sequence_to_schedule.observer);
  sequence_to_schedule.observer->OnCanScheduleSequence(
      std::move(sequence_to_schedule.sequence));
}

#if DCHECK_IS_ON()
bool TaskTracker::IsPostingBlockShutdownTaskAfterShutdownAllowed() {
  return false;
}
#endif

bool TaskTracker::HasIncompleteUndelayedTasksForTesting() const {
  return subtle::Acquire_Load(&num_incomplete_undelayed_tasks_) != 0;
}

bool TaskTracker::BeforePostTask(TaskShutdownBehavior shutdown_behavior) {
  if (shutdown_behavior == TaskShutdownBehavior::BLOCK_SHUTDOWN) {
    // BLOCK_SHUTDOWN tasks block shutdown between the moment they are posted
    // and the moment they complete their execution.
    const bool shutdown_started = state_->IncrementNumTasksBlockingShutdown();

    if (shutdown_started) {
      AutoSchedulerLock auto_lock(shutdown_lock_);

      // A BLOCK_SHUTDOWN task posted after shutdown has completed is an
      // ordering bug. This aims to catch those early.
      DCHECK(shutdown_event_);
      if (shutdown_event_->IsSignaled()) {
#if DCHECK_IS_ON()
// clang-format off
        // TODO(robliao): http://crbug.com/698140. Since the service thread
        // doesn't stop processing its own tasks at shutdown, we may still
        // attempt to post a BLOCK_SHUTDOWN task in response to a
        // FileDescriptorWatcher. Same is true for FilePathWatcher
        // (http://crbug.com/728235). Until it's possible for such services to
        // post to non-BLOCK_SHUTDOWN sequences which are themselves funneled to
        // the main execution sequence (a future plan for the post_task.h API),
        // this DCHECK will be flaky and must be disabled.
        // DCHECK(IsPostingBlockShutdownTaskAfterShutdownAllowed());
// clang-format on
#endif
        state_->DecrementNumTasksBlockingShutdown();
        return false;
      }
    }

    return true;
  }

  // A non BLOCK_SHUTDOWN task is allowed to be posted iff shutdown hasn't
  // started.
  return !state_->HasShutdownStarted();
}

bool TaskTracker::BeforeRunTask(TaskShutdownBehavior shutdown_behavior) {
  switch (shutdown_behavior) {
    case TaskShutdownBehavior::BLOCK_SHUTDOWN: {
      // The number of tasks blocking shutdown has been incremented when the
      // task was posted.
      DCHECK(state_->AreTasksBlockingShutdown());

      // Trying to run a BLOCK_SHUTDOWN task after shutdown has completed is
      // unexpected as it either shouldn't have been posted if shutdown
      // completed or should be blocking shutdown if it was posted before it
      // did.
      DCHECK(!state_->HasShutdownStarted() || !IsShutdownComplete());

      return true;
    }

    case TaskShutdownBehavior::SKIP_ON_SHUTDOWN: {
      // SKIP_ON_SHUTDOWN tasks block shutdown while they are running.
      const bool shutdown_started = state_->IncrementNumTasksBlockingShutdown();

      if (shutdown_started) {
        // The SKIP_ON_SHUTDOWN task isn't allowed to run during shutdown.
        // Decrement the number of tasks blocking shutdown that was wrongly
        // incremented.
        const bool shutdown_started_and_no_tasks_block_shutdown =
            state_->DecrementNumTasksBlockingShutdown();
        if (shutdown_started_and_no_tasks_block_shutdown)
          OnBlockingShutdownTasksComplete();

        return false;
      }

      return true;
    }

    case TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN: {
      return !state_->HasShutdownStarted();
    }
  }

  NOTREACHED();
  return false;
}

void TaskTracker::AfterRunTask(TaskShutdownBehavior shutdown_behavior) {
  if (shutdown_behavior == TaskShutdownBehavior::BLOCK_SHUTDOWN ||
      shutdown_behavior == TaskShutdownBehavior::SKIP_ON_SHUTDOWN) {
    const bool shutdown_started_and_no_tasks_block_shutdown =
        state_->DecrementNumTasksBlockingShutdown();
    if (shutdown_started_and_no_tasks_block_shutdown)
      OnBlockingShutdownTasksComplete();
  }
}

void TaskTracker::OnBlockingShutdownTasksComplete() {
  AutoSchedulerLock auto_lock(shutdown_lock_);

  // This method can only be called after shutdown has started.
  DCHECK(state_->HasShutdownStarted());
  DCHECK(shutdown_event_);

  shutdown_event_->Signal();
}

void TaskTracker::DecrementNumIncompleteUndelayedTasks() {
  const auto new_num_incomplete_undelayed_tasks =
      subtle::Barrier_AtomicIncrement(&num_incomplete_undelayed_tasks_, -1);
  DCHECK_GE(new_num_incomplete_undelayed_tasks, 0);
  if (new_num_incomplete_undelayed_tasks == 0) {
    {
      AutoSchedulerLock auto_lock(flush_lock_);
      flush_cv_->Signal();
    }
    CallFlushCallbackForTesting();
  }
}

scoped_refptr<Sequence> TaskTracker::ManageBackgroundSequencesAfterRunningTask(
    scoped_refptr<Sequence> just_ran_sequence,
    CanScheduleSequenceObserver* observer) {
  const TimeTicks next_task_sequenced_time =
      just_ran_sequence
          ? just_ran_sequence->GetSortKey().next_task_sequenced_time()
          : TimeTicks();
  PreemptedBackgroundSequence sequence_to_schedule;

  {
    AutoSchedulerLock auto_lock(background_lock_);

    DCHECK(preempted_background_sequences_.empty() ||
           num_scheduled_background_sequences_ ==
               max_num_scheduled_background_sequences_);
    --num_scheduled_background_sequences_;

    if (just_ran_sequence) {
      if (preempted_background_sequences_.empty() ||
          preempted_background_sequences_.top().next_task_sequenced_time >
              next_task_sequenced_time) {
        ++num_scheduled_background_sequences_;
        return just_ran_sequence;
      }

      preempted_background_sequences_.emplace(
          std::move(just_ran_sequence), next_task_sequenced_time, observer);
    }

    if (!preempted_background_sequences_.empty()) {
      sequence_to_schedule =
          GetPreemptedBackgroundSequenceToScheduleLockRequired();
    }
  }

  // |sequence_to_schedule.sequence| may be null if there was no preempted
  // background sequence.
  if (sequence_to_schedule.sequence)
    SchedulePreemptedBackgroundSequence(std::move(sequence_to_schedule));

  return nullptr;
}

void TaskTracker::CallFlushCallbackForTesting() {
  OnceClosure flush_callback;
  {
    AutoSchedulerLock auto_lock(flush_lock_);
    flush_callback = std::move(flush_callback_for_testing_);
  }
  if (flush_callback)
    std::move(flush_callback).Run();
}

}  // namespace internal
}  // namespace base