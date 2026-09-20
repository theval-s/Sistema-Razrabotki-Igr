#pragma once
#include <optional>
#include <vector>

namespace engine {

template <typename T>
struct HandleMap {

    struct Handle {
        uint32_t id;
        
        Handle() : id(std::numeric_limits<uint32_t>::max()) {}

        explicit operator bool() const {
            return id != std::numeric_limits<uint32_t>::max();
        }
    };
    
    static Handle EmptyHandle() {
        return Handle(std::numeric_limits<uint32_t>::max());
    }

    Handle<T> Add(T&& t) {
        storage_.push_back(std::move(t));
        return Handle(storage_.size() - 1);
    }

    T& Get(Handle h) {
        assert(h);
        return storage_[h.id];
    }

    std::optional<Handle> Find(const T& t) {
        auto res = std::find(storage_.begin(), storage_.end(), t);
        if (res != storage_.end()) {
            return Handle(std::distance(storage_.begin(), res));
        }
        return std::nullopt;
    }

private:
    std::vector<T> storage_{};
};

}
