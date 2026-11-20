#include "ops/Undo.h"

#include "core/Events.h"
#include "core/Profile.h"

#include <algorithm>
#include <ranges>

namespace lila
{
namespace
{
/** Reverts a step's own edits on redo and reapplies them on undo; shares them with that step. */
struct RevertAction : UndoAction
{
    std::vector<std::shared_ptr<UndoAction>> edits;

    void undo() override
    {
        for (auto& a : edits)
            a->redo();
    }
    void redo() override
    {
        for (auto& a : std::views::reverse(edits))
            a->undo();
    }
    bool selectionOnly() const override
    {
        return std::ranges::all_of(edits, [](const auto& a) { return a->selectionOnly(); });
    }
    void sharedBytes(UndoShared& s) const override
    {
        for (const auto& a : edits)
            a->sharedBytes(s);
    }
};
} // namespace

UndoStack& UndoStack::get()
{
    static UndoStack stack;
    return stack;
}

void UndoStack::openChunk(std::string label)
{
    if (mDepth++ == 0)
        mOpen = Chunk{std::move(label), {}};
}

void UndoStack::closeChunk()
{
    if (mDepth == 0 || --mDepth > 0)
        return;
    if (mOpen.actions.empty())
    {
        // A reopened step that now records nothing is gone from the stack.
        if (std::exchange(mReopened, false))
        {
            ++mSerial;
            notify();
        }
        return;
    }
    mReopened = false;
    Chunk c = std::move(mOpen);
    mOpen = {};
    push(std::move(c));
}

void UndoStack::cancelChunk()
{
    if (mDepth == 0)
        return;
    mDepth = 0;
    mReopened = false;
    Chunk c = std::move(mOpen);
    mOpen = {};
    mReplaying = true;
    for (auto& a : std::views::reverse(c.actions))
        a->undo();
    mReplaying = false;
    ++mSerial;
    notify();
}

void UndoStack::rollbackTo(size_t mark)
{
    if (mDepth == 0 || mark >= mOpen.actions.size())
        return;
    mReplaying = true;
    for (size_t i = mOpen.actions.size(); i-- > mark;)
        mOpen.actions[i]->undo();
    mReplaying = false;
    mOpen.actions.resize(mark);
}

bool UndoStack::openAmend(std::string label)
{
    LILA_ZONE();

    if (mDepth > 0 || mDone.empty())
        return false;
    const Chunk& newest = mDone.back();
    auto revert = std::make_shared<RevertAction>();
    revert->edits.assign(newest.actions.begin() + ptrdiff_t(newest.amends), newest.actions.end());
    mReplaying = true;
    revert->redo();
    mReplaying = false;
    openChunk(std::move(label));
    mOpen.actions.push_back(std::move(revert));
    mOpen.amends = 1;
    return true;
}

bool UndoStack::reopenNewest(std::string label)
{
    LILA_ZONE();

    if (mDepth > 0 || mDone.empty())
        return false;
    Chunk c = std::move(mDone.back());
    mDone.pop_back();
    account(c, false);
    mReplaying = true;
    for (size_t i = c.actions.size(); i > c.amends; --i)
        c.actions[i - 1]->undo();
    mReplaying = false;
    c.actions.resize(c.amends);
    mOpen = Chunk{std::move(label), std::move(c.actions)};
    mOpen.amends = c.amends;
    mDepth = 1;
    mReopened = true;
    return true;
}

uint64_t UndoStack::stateId() const
{
    for (const Chunk& c : std::views::reverse(mDone))
        if (!c.selectionOnly)
            return c.id;
    return mBaseId;
}

void UndoStack::push(Chunk c)
{
    c.id = mNextId++;
    c.selectionOnly = std::ranges::all_of(c.actions, [](const auto& a) { return a->selectionOnly(); });
    for (const auto& a : c.actions)
    {
        c.owned += a->bytes();
        a->sharedBytes(c.shared);
    }
    for (const Chunk& u : mUndone)
        account(u, false);
    mUndone.clear();
    account(c, true);
    mDone.push_back(std::move(c));
    ++mSerial;
    trim();
    notify();
}

void UndoStack::trim()
{
    // The newest step always stays, however large, so the last edit can be undone.
    while (!mDone.empty()
           && ((mLimit && mDone.size() > mLimit) || (mBudget && memoryBytes() > mBudget && mDone.size() > 1)))
    {
        if (!mDone.front().selectionOnly)
            mBaseId = mDone.front().id;
        account(mDone.front(), false);
        mDone.pop_front();
    }
}

void UndoStack::setLimit(size_t limit)
{
    mLimit = limit;
    size_t before = mDone.size();
    trim();
    if (mDone.size() != before)
    {
        ++mSerial;
        notify();
    }
}

void UndoStack::setByteBudget(size_t bytes)
{
    mBudget = bytes;
    size_t before = mDone.size();
    trim();
    if (mDone.size() != before)
    {
        ++mSerial;
        notify();
    }
}

void UndoStack::account(const Chunk& c, bool add)
{
    if (add)
        mOwnedBytes += c.owned;
    else
        mOwnedBytes -= c.owned;
    for (const auto& [key, bytes] : c.shared.items)
    {
        if (add)
        {
            SharedEntry& e = mShared[key];
            if (e.refs++ == 0)
            {
                e.bytes = bytes;
                mSharedBytes += bytes;
            }
            continue;
        }
        auto it = mShared.find(key);
        if (it != mShared.end() && --it->second.refs == 0)
        {
            mSharedBytes -= it->second.bytes;
            mShared.erase(it);
        }
    }
}

void UndoStack::resetAccounting()
{
    mShared.clear();
    mOwnedBytes = 0;
    mSharedBytes = 0;
}

std::vector<UndoStack::Step> UndoStack::steps() const
{
    std::vector<Step> out;
    out.reserve(mDone.size() + mUndone.size());
    for (const Chunk& c : mDone)
        out.push_back({c.label, c.selectionOnly});
    for (const Chunk& c : std::views::reverse(mUndone))
        out.push_back({c.label, c.selectionOnly});
    return out;
}

bool UndoStack::jumpTo(size_t position)
{
    LILA_ZONE();

    if (mDepth > 0 || position > mDone.size() + mUndone.size())
        return false;
    if (position == mDone.size())
        return true;
    mQuiet = true;
    while (mDone.size() > position)
        undo();
    while (mDone.size() < position)
        redo();
    mQuiet = false;
    notify();
    return true;
}

void UndoStack::notify()
{
    if (mQuiet)
        return;
    for (auto& fn : mListeners)
        fn();
    events::send({.type = events::Event::UndoStackChanged});
}

void UndoStack::record(std::unique_ptr<UndoAction> action)
{
    if (!mEnabled || mReplaying)
        return;
    if (mDepth > 0)
    {
        mOpen.actions.push_back(std::move(action));
        return;
    }
    // Edit made outside any command: give it a chunk of its own.
    Chunk c;
    c.actions.push_back(std::move(action));
    push(std::move(c));
}

std::shared_ptr<void> UndoStack::replayScope(size_t actions) const
{
    // Short steps keep their precise per-change notifications; a long one would rebuild its dependants per edit.
    constexpr size_t kLongStep = 8;
    return mReplayScope && actions >= kLongStep ? mReplayScope() : nullptr;
}

bool UndoStack::undo()
{
    LILA_ZONE();

    if (mDone.empty() || mDepth > 0)
        return false;
    Chunk c = std::move(mDone.back());
    mDone.pop_back();
    mReplaying = true;
    {
        const std::shared_ptr<void> scope = replayScope(c.actions.size());
        for (auto& a : std::views::reverse(c.actions))
            a->undo();
    }
    mReplaying = false;
    std::string label = c.label;
    mUndone.push_back(std::move(c));
    ++mSerial;
    notify();
    events::send({.type = events::Event::Undo, .name = std::move(label)});
    return true;
}

bool UndoStack::redo()
{
    LILA_ZONE();

    if (mUndone.empty() || mDepth > 0)
        return false;
    Chunk c = std::move(mUndone.back());
    mUndone.pop_back();
    mReplaying = true;
    {
        const std::shared_ptr<void> scope = replayScope(c.actions.size());
        for (auto& a : c.actions)
            a->redo();
    }
    mReplaying = false;
    std::string label = c.label;
    mDone.push_back(std::move(c));
    ++mSerial;
    notify();
    events::send({.type = events::Event::Redo, .name = std::move(label)});
    return true;
}

void UndoStack::clear()
{
    mDone.clear();
    mUndone.clear();
    resetAccounting();
    mBaseId = mNextId++;
    ++mSerial;
    notify();
}

void UndoStack::flush()
{
    mBaseId = stateId();
    mDone.clear();
    mUndone.clear();
    resetAccounting();
    ++mSerial;
    notify();
}

void UndoStack::flushPast()
{
    mBaseId = stateId();
    for (const Chunk& c : mDone)
        account(c, false);
    mDone.clear();
    ++mSerial;
    notify();
}

void UndoStack::flushFuture()
{
    for (const Chunk& c : mUndone)
        account(c, false);
    mUndone.clear();
    ++mSerial;
    notify();
}
} // namespace lila
