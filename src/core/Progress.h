#pragma once

#include "core/Export.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

/**
 * Progress of long operations, from any thread. A Scope is one operation (or one part of one): it has a title, an
 * optional total of units, a status line and a cancel flag. A Scope opened while another is current on the same thread
 * nests inside it and stands for `span` of the parent's units, so a nested operation moves the parent's bar smoothly.
 * A Scope with no parent is a root; several roots can run at once (a scene load on the main thread, an autosave on
 * another).
 *
 * Nothing listens by default and then a step is an atomic add. The UI installs a pump (called on the main thread while
 * long work runs there, so it can repaint and keep Windows from flagging the app as hung) and lilabatch a console
 * reporter.
 */
namespace lila::progress
{
enum Flags : uint32_t
{
    None = 0,
    /** Background work: never shown as a modal overlay, only as a small indicator. */
    Silent = 1,
    /**
     * Offers Abort: the operation polls checkCancel() and, when it throws, leaves the scene as it was (or, for one that
     * replaced the scene, empty). Commands roll back an undoable command's edits when it fails after a cancel.
     */
    Cancellable = 2,
};

/** Thrown by Scope::checkCancel. */
struct Cancelled : std::runtime_error
{
    Cancelled() : std::runtime_error("cancelled") {}
};

struct Node;

class LILA_FOUNDATION_API Scope
{
public:
    /** total 0 means unknown. span is how many of the parent's units this scope stands for. */
    explicit Scope(std::string title, double total = 0.0, uint32_t flags = None, double span = 1.0);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    /** Safe from any thread, so parallel loops can step a shared scope. */
    void step(double n = 1.0);
    void set(double done);
    void setTotal(double total);
    void setStatus(std::string text);
    /** Whether a Cancel button is offered now (read on the root): only while cancelling would leave the scene as it was. */
    void setCancellable(bool on);
    /**
     * Shows the root's modal card now rather than after the usual short delay. For main-thread work known to block
     * for a while before its next step, so the card paints before the wait instead of after it.
     */
    void showNow();
    /** True once the root this scope belongs to was cancelled. */
    bool cancelled() const;
    /** Throws Cancelled when cancelled. */
    void checkCancel() const;

    double done() const;
    double total() const;

private:
    friend LILA_FOUNDATION_API double fraction(const Scope& root);

    std::shared_ptr<Node> mNode;
    Node* mPrevCurrent = nullptr;
};

/** The innermost scope open on this thread, for handing to a worker (see Adopt). */
LILA_FOUNDATION_API std::shared_ptr<Node> current();

/** Makes `parent` the current scope of this thread, so scopes opened here nest under it. */
class LILA_FOUNDATION_API Adopt
{
public:
    explicit Adopt(std::shared_ptr<Node> parent);
    ~Adopt();
    Adopt(const Adopt&) = delete;
    Adopt& operator=(const Adopt&) = delete;

private:
    std::shared_ptr<Node> mParent;
    Node* mPrev = nullptr;
};

/** One running root, as the UI shows it. */
struct Snapshot
{
    uint64_t id = 0;
    std::string title;
    /** The innermost non-empty status, else the innermost nested title. */
    std::string status;
    /** 0..1 when determinate. */
    double fraction = 0.0;
    bool determinate = false;
    bool silent = false;
    bool cancellable = false;
    bool cancelled = false;
    /** Set by Scope::showNow: the card skips its delay. */
    bool immediate = false;
    double elapsed = 0.0; // seconds
    bool mainThread = false;
};
LILA_FOUNDATION_API std::vector<Snapshot> snapshot();

/** The overall fraction of one root's tree, for tests: (done + child fraction * child span) / total at each level. */
LILA_FOUNDATION_API double fraction(const Scope& root);

/**
 * The cancellation token converters poll: true once the root of this thread's innermost scope was cancelled (workers
 * under runOnWorker or Adopt see their caller's root). False outside any scope.
 */
LILA_FOUNDATION_API bool cancelRequested();
/** Throws Cancelled when cancelRequested(). */
LILA_FOUNDATION_API void checkCancel();
/** Cancelled roots closed on this thread so far: a failure that moved it came from a cancel. */
LILA_FOUNDATION_API uint64_t cancelledCount();

/** Cancels the root with this id (from a Snapshot). */
LILA_FOUNDATION_API void cancel(uint64_t id);

/** True while a non-silent root is open on the main thread: long work is running there under a modal overlay. */
LILA_FOUNDATION_API bool modal();

/**
 * Called on the main thread from steps of scopes open there, at most every `intervalMs`, and once when a root closes.
 * Not called again while it runs.
 */
LILA_FOUNDATION_API void setPump(std::function<void()> pump, int intervalMs = 30);

/** Called with every root's state at most every `intervalMs`, from whichever thread stepped; and with an empty list when the last root closes. */
LILA_FOUNDATION_API void setReporter(std::function<void(const std::vector<Snapshot>&)> reporter, int intervalMs = 100);

/** Runs the pump now if one is installed and this is the main thread. */
LILA_FOUNDATION_API void pumpNow();

/**
 * Runs fn on a worker thread, nested under this thread's current scope, while this thread keeps pumping. Exceptions
 * from fn are rethrown here. Runs fn inline when there is no pump or this isn't the main thread.
 */
LILA_FOUNDATION_API void runOnWorker(const std::function<void()>& fn);

/** "1.2 s", "2 min 05 s". */
LILA_FOUNDATION_API std::string formatElapsed(double seconds);

/** Single-line console rendering of the busiest root, as lilabatch prints it ("Converting x.mb  42%  Building meshes 120/263"). */
LILA_FOUNDATION_API std::string consoleLine(const std::vector<Snapshot>& roots, size_t width = 79);
} // namespace lila::progress
