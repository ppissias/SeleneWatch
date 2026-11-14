#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

struct MemoryUsage
{
    size_t used{0}, limit{0}, peak{0};
    size_t refused{0};
};

class MemoryLimitError : public std::runtime_error
{
public:
    MemoryLimitError() : std::runtime_error("RAM limit reached") {}
};

// One shared admission counter for image payload allocations. Copies refer
// to the same state, including buffers outliving a source or the Session.
// The default is unbounded, for standalone readers and constructed tests.
class MemoryBudget
{
public:
    MemoryBudget() = default;
    explicit MemoryBudget(size_t limit);
    bool acquire(size_t bytes) const;
    void release(size_t bytes) const;
    bool setLimit(size_t bytes);
    [[nodiscard]] MemoryUsage usage() const;
    bool operator==(const MemoryBudget&) const = default;
private:
    struct State;
    std::shared_ptr<State> state_;
};

// A standard allocator keeps capacity (including growth and temporary
// copies) charged until deallocation. Published frames sharing one vector
// count once; copying its pixels makes a separate, charged allocation.
template<class T> class BudgetAllocator
{
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    MemoryBudget budget;
    BudgetAllocator() = default;
    explicit BudgetAllocator(MemoryBudget value) : budget(std::move(value)) {}
    template<class U> BudgetAllocator(const BudgetAllocator<U>& value) : budget(value.budget) {}
    T* allocate(size_t count)
    {
        if (count > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
        const size_t bytes = count * sizeof(T);
        // Rebound allocators may hold standard-library debug bookkeeping.
        // The ceiling covers numeric image/analysis storage, not container metadata.
        if constexpr (std::is_arithmetic_v<T>)
            if (!budget.acquire(bytes)) throw MemoryLimitError();
        try { return std::allocator<T>{}.allocate(count); }
        catch (...) { if constexpr (std::is_arithmetic_v<T>) budget.release(bytes); throw; }
    }
    void deallocate(T* data, size_t count)
    {
        std::allocator<T>{}.deallocate(data, count);
        if constexpr (std::is_arithmetic_v<T>) budget.release(count * sizeof(T));
    }
    template<class U> bool operator==(const BudgetAllocator<U>& value) const { return budget == value.budget; }
};

template<class T> using BudgetVector = std::vector<T, BudgetAllocator<T>>;
