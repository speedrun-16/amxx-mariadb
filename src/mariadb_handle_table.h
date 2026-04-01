#ifndef AMXX_MARIADB_HANDLE_TABLE_H
#define AMXX_MARIADB_HANDLE_TABLE_H

#include <memory>
#include <mutex>
#include <vector>

// ============================================================================
// HANDLE TABLE
// ============================================================================

// thread-safe integer-keyed slot table for shared_ptr values
// handles are 1-based (index + 1); 0 and negatives are invalid
// freed slots are recycled via a free list
template <typename T>
class handle_table
{
public:
    // inserts value, returns a handle (>= 1) for later lookup
    cell create(const std::shared_ptr<T>& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!free_list_.empty())
        {
            const auto index = free_list_.back();
            free_list_.pop_back();
            slots_[index] = value;
            return static_cast<cell>(index + 1);
        }

        slots_.push_back(value);
        return static_cast<cell>(slots_.size());
    }

    // returns the value for handle, or nullptr if handle is invalid
    std::shared_ptr<T> get(cell handle) const
    {
        if (handle <= 0)
        {
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto index = static_cast<size_t>(handle - 1);
        if (index >= slots_.size())
        {
            return nullptr;
        }

        return slots_[index];
    }

    // releases the slot for handle, returns false if handle was invalid
    bool destroy(cell handle)
    {
        if (handle <= 0)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto index = static_cast<size_t>(handle - 1);
        if (index >= slots_.size() || !slots_[index])
        {
            return false;
        }

        slots_[index].reset();
        free_list_.push_back(index);
        return true;
    }

    // drops all slots and the free list
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_.clear();
        free_list_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<T>> slots_;
    std::vector<size_t> free_list_;
};

#endif
