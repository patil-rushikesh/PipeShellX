#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace psx::runtime {

// A Lamport logical clock: a counter that gives a total order over events
// consistent with happens-before, without relying on wall-clock time from
// independent machines. See docs/ds-project/01-lamport-clocks.md for how
// this plugs into the psx/1 controller/node exchange.
//
// Not thread-safe by design: every reactor-owned object in this codebase
// (Reactor, NativeController, NodeServer, NodeStageRunner, ...) runs on a
// single thread, so a LamportClock member follows the same rule. If a
// future caller genuinely needs cross-thread access, that caller owns the
// synchronization -- this class deliberately stays a bare counter to match
// the rest of psx::runtime.
class LamportClock {
public:
    LamportClock() = default;
    explicit LamportClock(std::uint64_t initial) : counter_(initial) {}

    // Largest value the clock will ever hold. The counter saturates here
    // instead of wrapping: a wrap to 0 would make a later event look older
    // than an earlier one, silently breaking happens-before for every reader.
    // A peer can push any u64 at us (OPEN v2 carries it off the wire), so the
    // ceiling is reachable by a single hostile or buggy message, not just in
    // theory. Once saturated the clock no longer advances; saturated() lets a
    // caller detect that and refuse to order further events.
    static constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

    // A local event: advance the clock by one and return the new value.
    // Call this immediately before an outgoing message is built, or for any
    // internal event you want ordered relative to messages.
    std::uint64_t tick() noexcept {
        if (counter_ != kMax) {
            ++counter_;
        }
        return counter_;
    }

    // A receive event carrying a peer's timestamp: adopt the Lamport rule
    // counter = max(local, received) + 1, and return the new value. Pass the
    // timestamp exactly as received off the wire; do not pre-adjust it.
    std::uint64_t observe(std::uint64_t received) noexcept {
        const std::uint64_t base = std::max(counter_, received);
        counter_ = base == kMax ? kMax : base + 1;
        return counter_;
    }

    // True once the clock has hit kMax and stopped advancing.
    bool saturated() const noexcept { return counter_ == kMax; }

    // Current value with no side effect. Useful for logging/snapshotting
    // without perturbing the clock.
    std::uint64_t value() const noexcept { return counter_; }

private:
    std::uint64_t counter_ = 0;
};

} // namespace psx::runtime
