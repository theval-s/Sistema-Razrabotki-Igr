#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <engine/core/ResourceID.hpp>
#include <engine/assets/AssetCommon.hpp>
#include <engine/assets/AssetHandle.hpp>

#include <entt/resource/resource.hpp>

namespace engine {

/// Records of one asset type, addressed by normalized name.
///
/// A value is handed out as an entt::resource, so it outlives the record it was published
/// from: a slot is only recycled after a load attempt that never reached Ready, and callers
/// that still hold the resource keep reading the same asset either way.
///
/// The AssetHandle used to address a record carries a generation: one taken before such a
/// recycle resolves to "stale" instead of to whatever asset took the slot.
template <typename T>
class AssetTable {
public:
    struct Record {
        AssetState state = AssetState::Unloaded;
        ResourceID id = 0;
        uint32_t generation = 0;
        AssetDesc desc;
        entt::resource<T> value;
        std::string error;
    };

    /// Slot of an already known asset; Invalid() for a name that was never requested and
    /// for a name whose id belongs to a different asset (see FindCollision).
    AssetHandle<T> Find(const std::string_view normalizedName) const {
        const auto it = index_.find(ToResourceID(normalizedName));
        if (it == index_.end() || records_[it->second]->desc.name != normalizedName) {
            return AssetHandle<T>::Invalid();
        }
        return {it->second, records_[it->second]->generation};
    }

    /// The record squatting on this id, or nullptr when the id is free or really ours.
    const Record* FindCollision(const std::string_view name) const {
        const auto it = index_.find(ToResourceID(name));
        if (it == index_.end()) {
            return nullptr;
        }
        const Record* record = records_[it->second].get();
        return record->desc.name == name ? nullptr : record;
    }

    /// Reuses the slot of a known name, otherwise takes a free one. Invalid() when the
    /// name collides with a different asset: the caller reports it, nothing is overwritten.
    AssetHandle<T> Acquire(AssetDesc&& desc) {
        if (const auto known = Find(desc.name)) {
            return known;
        }
        if (index_.contains(ToResourceID(desc.name))) {
            return AssetHandle<T>::Invalid();
        }

        const ResourceID id = ToResourceID(desc.name);
        uint32_t slot;
        if (!freeSlots_.empty()) {
            slot = freeSlots_.back();
            freeSlots_.pop_back();
            const auto generation = records_[slot]->generation; // Fail already invalidated the old handle
            *records_[slot] = Record{};
            records_[slot]->generation = generation; // never reset generations across successive retries
        } else {
            slot = static_cast<uint32_t>(records_.size());
            records_.push_back(std::make_unique<Record>());
        }
        records_[slot]->id = id;
        records_[slot]->desc = std::move(desc);
        index_[id] = slot;
        return {slot, records_[slot]->generation};
    }

    Record* Get(const AssetHandle<T> handle) {
        if (!handle || handle.slot >= records_.size()) {
            return nullptr;
        }
        Record* record = records_[handle.slot].get();
        return record->generation == handle.generation ? record : nullptr;
    }

    const Record* Get(const AssetHandle<T> handle) const {
        return const_cast<AssetTable*>(this)->Get(handle);
    }

    /// Stores the loaded value and marks the record Ready; returns an empty resource for a
    /// stale handle. Re-publishing (a second shader stage on the same record) swaps in a new
    /// value: whoever took the resource earlier keeps reading the asset as it was then.
    entt::resource<T> Publish(const AssetHandle<T> handle, T&& value) {
        Record* record = Get(handle);
        if (record == nullptr) {
            return {};
        }
        record->value = entt::resource<T>{std::make_shared<T>(std::move(value))};
        record->state = AssetState::Ready;
        record->error.clear();
        return record->value;
    }

    void MarkLoading(const AssetHandle<T> handle) {
        if (Record* record = Get(handle)) {
            record->state = AssetState::Loading;
            record->error.clear();
        }
    }

    /// A record that already reached Ready keeps its slot forever; a never-ready one is
    /// handed back so a corrected name (or a plain retry) can take it.
    void Fail(const AssetHandle<T> handle, std::string error) {
        Record* record = Get(handle);
        if (record == nullptr || record->state == AssetState::Ready) {
            return;
        }
        record->state = AssetState::Failed;
        record->error = std::move(error);
        index_.erase(record->id);
        ++record->generation;
        freeSlots_.push_back(handle.slot);
    }

    [[nodiscard]] std::size_t Size() const noexcept {
        return records_.size();
    }

    template <class Fn>
    void ForEachReady(Fn&& fn) const {
        for (const auto& record : records_) {
            if (record->state == AssetState::Ready) fn(*record->value);
        }
    }

    template <class Fn>
    void ForEachReadyRecord(Fn&& fn) const {
        for (const auto& record : records_) {
            if (record->state == AssetState::Ready) fn(*record);
        }
    }

    // Releases records without resetting generations: an old handle must remain
    // invalid even when a subsequent scene loads the same name into the same slot.
    void EvictAll() {
        index_.clear();
        freeSlots_.clear();
        for (std::size_t i = 0; i < records_.size(); ++i) {
            auto& record = *records_[i];
            ++record.generation;
            record.state = AssetState::Unloaded;
            record.value = {};
            record.desc = {};
            record.error.clear();
            freeSlots_.push_back(static_cast<uint32_t>(i));
        }
    }

private:
    std::vector<std::unique_ptr<Record>> records_;
    std::unordered_map<ResourceID, uint32_t> index_;
    std::vector<uint32_t> freeSlots_;
};

}  // namespace engine
