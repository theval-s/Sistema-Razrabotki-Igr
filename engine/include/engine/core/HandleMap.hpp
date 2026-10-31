#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace engine {

/// Simple generational handle map
/// Stores all values in flat vector, removed items leave "holes" that are then reused. 
/// Generation < 0 represents empty slot

template <typename T>
struct HandleMap {
    HandleMap() = default;
    HandleMap(const HandleMap&) = delete;
    HandleMap& operator=(const HandleMap&) = delete;
    HandleMap(HandleMap&&) noexcept = default;
    HandleMap& operator=(HandleMap&&) noexcept = default;

    struct Handle {
        uint32_t id = std::numeric_limits<uint32_t>::max();
        int32_t generation = 0;

        Handle() = default;

        explicit Handle(const uint32_t value, const int32_t gen) : id(value), generation(gen) {
        }

        explicit operator bool() const noexcept {
            return id != std::numeric_limits<uint32_t>::max();
        }

        bool operator==(const Handle&) const = default;
    };

    static Handle EmptyHandle() {
        return {};
    }

    Handle Add(T&& t) {
        if (!freeSlots_.empty()) {
            const uint32_t id = freeSlots_.back();
            auto& slot = storage_[id];

            assert(!slot.Used());
            const int32_t generation = -(slot.generation + 1);

            std::construct_at(&slot.val.value, std::move(t));
            slot.generation = generation;
            freeSlots_.pop_back();
            return Handle{id, generation};
        }

        const uint32_t id = static_cast<uint32_t>(storage_.size());
        storage_.emplace_back(std::move(t));
        return Handle{id, 0};
    }

    [[nodiscard]] bool Contains(Handle h) const noexcept {
        return h.id >= 0 && static_cast<std::size_t>(h.id) < storage_.size()
            && storage_[h.id].Used() && storage_[h.id].generation == h.generation;
    }
    
    T* TryGet(Handle h) noexcept {
        return Contains(h) ? &storage_[h.id].val.value : nullptr;
    }

    const T* TryGet(Handle h) const noexcept {
        return Contains(h) ? &storage_[h.id].val.value : nullptr;
    }

    T& Get(Handle h) {
        assert(Contains(h));
        return storage_[h.id].val.value;
    }

    const T& Get(Handle h) const {
        assert(Contains(h));
        return storage_[h.id].val.value;
    }

    [[nodiscard]] std::size_t Size() const noexcept {
        return storage_.size() - freeSlots_.size();
    }

    template <typename F>
    void ForEach(F&& visit) const {
        for (const auto& slot : storage_) if (slot.Used()) visit(slot.val.value);
    }

    /// Handles of all stored values, in slot order. A copy, so values may be added or removed
    /// while going through it; check each with Contains() or TryGet().
    [[nodiscard]] std::vector<Handle> Handles() const {
        std::vector<Handle> handles;
        Handles(handles);
        return handles;
    }

    /// Same, into `handles` (replacing its contents), so a caller doing this every frame can keep
    /// reusing one buffer.
    void Handles(std::vector<Handle>& handles) const {
        handles.clear();
        handles.reserve(Size());
        for (std::size_t id = 0; id < storage_.size(); ++id) {
            if (storage_[id].Used()) {
                handles.emplace_back(static_cast<uint32_t>(id), storage_[id].generation);
            }
        }
    }
    
    bool Remove(Handle h) {
        if (!Contains(h)) {
            return false;
        }

        auto& slot = storage_[h.id];
        const int32_t generation = slot.generation;
        freeSlots_.push_back(h.id);
        std::destroy_at(&slot.val.value);

        if (generation == std::numeric_limits<int32_t>::max()) {
            // Restart generation cycle
            slot.generation = -1;
        } else {
            slot.generation = -generation - 2;
        }
        
        return true;
    }

    std::optional<Handle> Find(const T& t) {
        auto res = std::find_if(storage_.begin(), storage_.end(), [&](const Slot & slot) -> bool{
            return slot.Used() && slot.val.value == t;
        });
        if (res != storage_.end()) {
            return Handle(static_cast<uint32_t>(std::distance(storage_.begin(), res)), res->generation);
        }
        return std::nullopt;
    }

private:
    struct Slot {
        //empty by default
        union Storage {
            T value;

            Storage() noexcept {
            }

            ~Storage() noexcept {
            }
        } val;

        //todo: technically this thing could waste a lot of memory due to padding, we could move generations to separate vector nearby
        int32_t generation = -1;

        [[nodiscard]] bool Used() const noexcept {
            return generation >= 0;
        }

        explicit Slot(T&& value) noexcept {
            std::construct_at(&val.value, std::move(value));
            generation = 0;
        }

        Slot(Slot&& other) noexcept : generation(other.generation) {
            if (Used()) {
                std::construct_at(&val.value, std::move(other.val.value));
            }
        }

        ~Slot() noexcept {
            if (Used()) {
                std::destroy_at(&val.value);
            }
        }

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
        Slot& operator=(Slot&&) = delete;
    };

    std::vector<Slot> storage_{};
    std::vector<uint32_t> freeSlots_;
};

}
