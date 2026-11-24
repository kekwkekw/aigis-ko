#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aigiskr::core {
// Pointer-sized keys are identities only: this registry never owns or dereferences
// an engine object. Clear/destruction/text writes invalidate a binding. A serial
// prevents a completion from consuming a newer binding at a recycled address.
class PendingTextRefresh {
public:
    struct Binding {
        std::string source, key;
        uint32_t thread = 0;
        uint64_t revision = 0, serial = 0;
        std::array<uint32_t, 10> initialColors{};
    };
    explicit PendingTextRefresh(size_t limit = 0) : limit_(limit) {}
    bool Track(uintptr_t identity, std::string_view source, std::string_view key, uint32_t thread,
               const std::array<uint32_t, 10>& initialColors = {}) {
        std::lock_guard lock(mutex_);
        if (limit_ && !entries_.contains(identity) && entries_.size() >= limit_) return false;
        entries_[identity] = {std::string(source), std::string(key), thread, 0, ++serial_, initialColors};
        return true;
    }
    bool Forget(uintptr_t identity) {
        std::lock_guard lock(mutex_);
        return entries_.erase(identity) != 0;
    }
    bool Candidate(uintptr_t identity, uint32_t thread, uint64_t revision, Binding& out) {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(identity);
        if (it == entries_.end() || it->second.thread != thread || it->second.revision == revision) return false;
        it->second.revision = revision;
        out = it->second;
        return true;
    }
    bool Consume(uintptr_t identity, uint64_t serial, std::string_view currentSource) {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(identity);
        if (it == entries_.end() || it->second.serial != serial) return false;
        // Single writes may retain their logical source or deliberately clear it
        // after emitting glyphs. Actual subsequent writes/clear/destroy invalidate
        // this generation; a different nonempty source is never accepted.
        const bool unchanged = currentSource.empty() || it->second.source == currentSource;
        entries_.erase(it);
        return unchanged;
    }
    size_t Size() const { std::lock_guard lock(mutex_); return entries_.size(); }
    void Clear() { std::lock_guard lock(mutex_); entries_.clear(); }
private:
    mutable std::mutex mutex_;
    std::unordered_map<uintptr_t, Binding> entries_;
    size_t limit_;
    uint64_t serial_ = 0;
};
}
