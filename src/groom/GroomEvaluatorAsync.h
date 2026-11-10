// Adapted from GroomLab's async_evaluation.h (github.com/thekesslereffect/groomlab, Apache-2.0,
// see src/groom/groomlab/LICENSE and NOTICE).
#pragma once

#include "groom/GroomEvaluator.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <thread>

namespace lila::groom
{
/**
 * A GroomEvaluator evaluated off the caller's thread: a host whose UI must not wait for an evaluation starts it here and
 * takes the result when it is ready, drawing what it has meanwhile. From start() until wait() returns the evaluator
 * belongs to the worker; one evaluation runs at a time, in the order started.
 */
class AsyncEvaluation
{
public:
    enum class State
    {
        /** Nothing started, or the last result taken: the evaluator is the host's. */
        Idle,
        Running,
        /** Evaluated, not taken yet: wait() returns at once. */
        Done,
    };
    /** Runs on the worker before the evaluation (setters whose parsing shouldn't hold up the host). */
    using Prepare = std::function<void(GroomEvaluator&)>;

    explicit AsyncEvaluation(GroomEvaluator& evaluator) : mEvaluator(evaluator) {}
    /** Waits for an evaluation still running (its result dropped, anything it threw swallowed). */
    ~AsyncEvaluation();
    AsyncEvaluation(const AsyncEvaluation&) = delete;
    AsyncEvaluation& operator=(const AsyncEvaluation&) = delete;

    /** Evaluates `request` (after `prepare`) on a worker thread. One started before and not taken is waited for first
     * and its result dropped: each goes on from the evaluator as the one before left it. */
    void start(const GroomEvaluator::Request& request, Prepare prepare = {});
    State state() const { return mState.load(std::memory_order_acquire); }
    bool pending() const { return state() != State::Idle; }
    bool ready() const { return state() == State::Done; }
    /** Waits and hands the evaluator back: its result, or null when nothing was started. What escaped the worker is
     * rethrown here, once. */
    const GroomEvaluator::Result* wait();
    double lastMilliseconds() const { return mLastMs; }
    std::uint64_t evaluations() const { return mEvaluations; }
    /** Threads the worker's evaluation takes from the next start (0: every core). */
    void setThreads(std::size_t threads) { mThreads = threads; }
    std::size_t threads() const { return mThreads; }

private:
    GroomEvaluator& mEvaluator;
    std::size_t mThreads = 0;
    std::thread mWorker;
    std::atomic<State> mState{State::Idle};
    const GroomEvaluator::Result* mResult = nullptr;
    std::exception_ptr mError;
    double mLastMs = 0.0;
    std::uint64_t mEvaluations = 0;
};

/**
 * Detached evaluations run in the order started, each on a worker thread that first waits for the one before: start()
 * never waits, and the evaluator stays the host's throughout.
 */
class DetachedRunner
{
public:
    DetachedRunner() = default;
    /** Waits for them all. */
    ~DetachedRunner();
    DetachedRunner(const DetachedRunner&) = delete;
    DetachedRunner& operator=(const DetachedRunner&) = delete;

    /** Runs `run` once the one started before has run. Throws when no thread can be made. */
    void start(std::shared_ptr<DetachedEvaluation> run);
    /** The last started and not taken (null: none), for a chained detach. */
    const std::shared_ptr<DetachedEvaluation>& last() const { return mLast; }
    bool pending() const { return mLast != nullptr; }
    bool ready() const { return mLast && mLast->ran(); }
    /** Waits for every one started; the last, null when none is pending. */
    std::shared_ptr<DetachedEvaluation> wait();
    void setThreads(std::size_t threads) { mThreads = threads; }
    std::uint64_t started() const { return mStarted; }

private:
    std::size_t mThreads = 0;
    /** The last one's; it joins the one before. */
    std::thread mWorker;
    std::shared_ptr<DetachedEvaluation> mLast;
    std::uint64_t mStarted = 0;
};
} // namespace lila::groom
