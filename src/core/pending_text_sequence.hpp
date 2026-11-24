#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aigiskr::core {
// One dialogue turn can append several independently translated lines to a
// single draw object. Keep every line, including already translated/static ones.
// Identities are never dereferenced here; native clear/destroy ends the lifetime.
class PendingTextSequence {
public:
    struct Part { std::string source, key, rendered; bool pending = false; };
    struct Binding {
        std::vector<Part> parts;
        std::array<uint32_t, 10> initialColors{};
        uint32_t thread = 0;
        uint64_t revision = 0, serial = 0;
        bool valid = true;
    };
    explicit PendingTextSequence(size_t limit = 0, size_t maxParts = 0, size_t maxBytes = 0)
        : limit_(limit), maxParts_(maxParts), maxBytes_(maxBytes) {}

    bool Append(uintptr_t id, bool emptyDraw, uint32_t thread, Part part,
                const std::array<uint32_t, 10>& colors) {
        std::lock_guard lock(mutex_);
        auto it = entries_.find(id);
        if (it == entries_.end()) {
            if (!emptyDraw || (limit_ && entries_.size() >= limit_)) return false;
            it = entries_.emplace(id, Binding{}).first;
            it->second.thread = thread;
            it->second.initialColors = colors;
        }
        auto& b = it->second;
        if (!b.valid) return false;
        b.parts.push_back(std::move(part));
        b.serial = ++serial_;
        b.revision = 0; // includes a completion racing with this append
        if (b.thread != thread || (maxParts_ && b.parts.size() > maxParts_) ||
            (maxBytes_ && Bytes(b.parts) > maxBytes_)) {
            // Keep a tombstone until a real clear: later appends must not replay
            // an incomplete prefix, even if the rejected line emitted no glyphs.
            b.valid = false;
            b.parts.clear();
            return false;
        }
        return true;
    }
    bool Candidate(uintptr_t id, uint32_t thread, uint64_t revision, Binding& out) {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(id);
        if (it == entries_.end()) return false;
        auto& b = it->second;
        if (!b.valid || b.thread != thread || b.revision == revision) return false;
        b.revision = revision;
        for (const auto& p : b.parts) if (p.pending) { out = b; return true; }
        return false;
    }
    template<class Lookup> static bool Resolve(Binding& b, Lookup lookup) {
        bool changed = false;
        for (auto& p : b.parts) {
            std::string target;
            if (p.pending && lookup(p.key, target)) {
                p.rendered = std::move(target);
                p.pending = false;
                changed = true;
            }
        }
        return changed;
    }
    bool Commit(uintptr_t id, const Binding& candidate, bool ownerEmpty) {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(id);
        if (it == entries_.end() || it->second.serial != candidate.serial) return false;
        if (!ownerEmpty || (maxBytes_ && Bytes(candidate.parts) > maxBytes_)) {
            it->second.valid = false;
            it->second.parts.clear();
            return false;
        }
        it->second.parts = candidate.parts;
        return true;
    }
    bool Forget(uintptr_t id) { std::lock_guard lock(mutex_); return entries_.erase(id) != 0; }
    void Block(uintptr_t id) {
        std::lock_guard lock(mutex_);
        if (limit_ && !entries_.contains(id) && entries_.size() >= limit_) return;
        auto& b = entries_[id];
        b.valid = false;
        b.serial = ++serial_;
        b.parts.clear();
    }
    size_t Size() const { std::lock_guard lock(mutex_); return entries_.size(); }
    void Clear() { std::lock_guard lock(mutex_); entries_.clear(); }
private:
    static size_t Bytes(const std::vector<Part>& parts) {
        size_t bytes = 0;
        for (const auto& p : parts) bytes += p.source.size() + p.key.size() + p.rendered.size();
        return bytes;
    }
    mutable std::mutex mutex_;
    std::unordered_map<uintptr_t, Binding> entries_;
    size_t limit_, maxParts_, maxBytes_;
    uint64_t serial_ = 0;
};
}
