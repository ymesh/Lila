// Adapted from GroomLab's async_evaluation.cpp (github.com/thekesslereffect/groomlab, Apache-2.0,
// see src/groom/groomlab/LICENSE and NOTICE).
#include "groom/GroomEvaluatorAsync.h"

#include <groomlab/parallel.h>

#include <chrono>
#include <utility>

namespace lila::groom
{
AsyncEvaluation::~AsyncEvaluation()
{
    try
    {
        wait();
    }
    catch (...)
    {
    }
}

void AsyncEvaluation::start(const GroomEvaluator::Request& request, Prepare prepare)
{
    if (pending())
    {
        try
        {
            wait();
        }
        catch (...)
        {
        }
    }
    mResult = nullptr;
    mError = nullptr;
    mState.store(State::Running, std::memory_order_release);
    try
    {
        mWorker = std::thread([this, request, prepare = std::move(prepare), threads = mThreads] {
            const auto started = std::chrono::steady_clock::now();
            struct Work
            {
                AsyncEvaluation* self;
                const GroomEvaluator::Request* request;
                const Prepare* prepare;
            } work{this, &request, &prepare};
            groomlab::runWithThreads(
                threads,
                [](void* context) {
                    const Work& w = *static_cast<const Work*>(context);
                    try
                    {
                        if (*w.prepare)
                            (*w.prepare)(w.self->mEvaluator);
                        w.self->mResult = &w.self->mEvaluator.evaluate(*w.request);
                    }
                    catch (...)
                    {
                        w.self->mError = std::current_exception();
                    }
                },
                &work);
            mLastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            mState.store(State::Done, std::memory_order_release);
        });
    }
    catch (...)
    {
        mState.store(State::Idle, std::memory_order_release);
        throw;
    }
}

const GroomEvaluator::Result* AsyncEvaluation::wait()
{
    if (!pending())
        return nullptr;
    if (mWorker.joinable())
        mWorker.join();
    mState.store(State::Idle, std::memory_order_release);
    ++mEvaluations;
    if (mError)
    {
        std::exception_ptr error = std::exchange(mError, nullptr);
        mResult = nullptr;
        std::rethrow_exception(error);
    }
    return std::exchange(mResult, nullptr);
}

DetachedRunner::~DetachedRunner()
{
    if (mWorker.joinable())
        mWorker.join();
}

void DetachedRunner::start(std::shared_ptr<DetachedEvaluation> run)
{
    auto before = std::make_shared<std::thread>(std::move(mWorker));
    try
    {
        mWorker = std::thread([before, run, threads = mThreads] {
            // In order: this one may go on from the one before.
            if (before->joinable())
                before->join();
            groomlab::runWithThreads(threads, [](void* r) { static_cast<DetachedEvaluation*>(r)->run(); }, run.get());
        });
    }
    catch (...)
    {
        if (before->joinable())
            before->join();
        throw;
    }
    mLast = std::move(run);
    ++mStarted;
}

std::shared_ptr<DetachedEvaluation> DetachedRunner::wait()
{
    if (mWorker.joinable())
        mWorker.join();
    return std::exchange(mLast, nullptr);
}
} // namespace lila::groom
