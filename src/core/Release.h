#pragma once

#include "core/Export.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

/**
 * Freeing a replaced scene's data (gigabytes) on one background thread that keeps off the frames. The process heap has
 * one lock: a thread freeing gigabytes holds it for most of its run, and a frame being built waits on it at every
 * allocation. The releaser frees a piece at a time and holds off while a Hold lives (a viewport building a frame);
 * with no frame built for a moment (headless, a long command) it frees at normal priority.
 */
namespace lila::release
{
/** Something to free in steps; step() frees one piece and returns true while pieces remain. */
struct Piece
{
    virtual ~Piece() = default;
    virtual bool step() = 0;
};

/** Queues a piece for the releaser thread. */
LILA_FOUNDATION_API void enqueue(std::unique_ptr<Piece> piece);

/** While one lives (on any thread), the releaser frees nothing more until it ends. */
class LILA_FOUNDATION_API Hold
{
public:
    Hold();
    ~Hold();
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
};

/** Blocks until everything queued is freed. */
LILA_FOUNDATION_API void drain();

/** Pieces queued or being freed. */
LILA_FOUNDATION_API size_t pending();

/** Values queued with later() or laterEach() that are entirely freed so far: caches holding weak references can sweep when it moves. */
LILA_FOUNDATION_API uint64_t finished();

/** Frees value on the releaser thread in one piece. */
template <class T> void later(T&& value)
{
    struct One : Piece
    {
        std::decay_t<T> v;
        explicit One(T&& x) : v(std::forward<T>(x)) {}
        bool step() override
        {
            [[maybe_unused]] std::decay_t<T> dead = std::move(v);
            return false;
        }
    };
    enqueue(std::make_unique<One>(std::forward<T>(value)));
}

/** Frees a vector's elements on the releaser thread, one element a piece, so a frame can start between any two. */
template <class T> void laterEach(std::vector<T>&& values)
{
    struct Each : Piece
    {
        std::vector<T> v;
        explicit Each(std::vector<T>&& x) : v(std::move(x)) {}
        bool step() override
        {
            if (!v.empty())
                v.pop_back();
            if (!v.empty())
                return true;
            std::vector<T>().swap(v);
            return false;
        }
    };
    if (!values.empty())
        enqueue(std::make_unique<Each>(std::move(values)));
}
} // namespace lila::release
