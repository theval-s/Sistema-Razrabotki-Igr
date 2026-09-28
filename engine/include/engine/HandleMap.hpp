#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace engine {

template <typename T>
struct HandleMap {

    struct Handle {
        uint32_t id = std::numeric_limits<uint32_t>::max();
        
        Handle() = default;
        explicit Handle(const uint32_t value) : id(value) {}

        explicit operator bool() const noexcept {
            return id != std::numeric_limits<uint32_t>::max();
        }

        bool operator==(const Handle&) const = default;
    };
    
    static Handle EmptyHandle() {
        return Handle(std::numeric_limits<uint32_t>::max());
    }

    Handle Add(T&& t) {
        storage_.push_back(std::move(t));
        return Handle(static_cast<uint32_t>(storage_.size() - 1));
    }

    T& Get(Handle h) {
        assert(h);
        return storage_[h.id];
    }

    std::optional<Handle> Find(const T& t) {
        auto res = std::find(storage_.begin(), storage_.end(), t);
        if (res != storage_.end()) {
            return Handle(static_cast<uint32_t>(std::distance(storage_.begin(), res)));
        }
        return std::nullopt;
    }

private:
    std::vector<T> storage_{};
};

}
