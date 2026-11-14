#include "memory_budget.h"

#include <algorithm>
#include <cassert>
#include <mutex>

struct MemoryBudget::State
{
    explicit State(size_t ceiling) : limit(ceiling) {}
    std::mutex mutex;
    size_t used{0}, limit, peak{0}, refused{0};
};

MemoryBudget::MemoryBudget(size_t limit) : state_(std::make_shared<State>(limit)) {}

bool MemoryBudget::acquire(size_t bytes) const
{
    if (!state_) return true;
    std::lock_guard lock(state_->mutex);
    if (bytes > state_->limit - state_->used) { ++state_->refused; return false; }
    state_->used += bytes;
    state_->peak = std::max(state_->peak, state_->used);
    return true;
}

void MemoryBudget::release(size_t bytes) const
{
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    assert(bytes <= state_->used);
    state_->used -= bytes;
}

bool MemoryBudget::setLimit(size_t bytes)
{
    if (!state_) { state_ = std::make_shared<State>(bytes); return true; }
    std::lock_guard lock(state_->mutex);
    if (bytes < state_->used) return false;
    state_->limit = bytes;
    state_->peak = state_->used;
    state_->refused = 0;
    return true;
}

MemoryUsage MemoryBudget::usage() const
{
    if (!state_) return {0, std::numeric_limits<size_t>::max(), 0, 0};
    std::lock_guard lock(state_->mutex);
    return {state_->used, state_->limit, state_->peak, state_->refused};
}
