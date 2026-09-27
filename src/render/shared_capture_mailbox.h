#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace mecvr::render {

struct AdapterLuid {
  std::uint32_t low = 0;
  std::int32_t high = 0;

  friend constexpr bool operator==(AdapterLuid left, AdapterLuid right) {
    return left.low == right.low && left.high == right.high;
  }
  friend constexpr bool operator!=(AdapterLuid left, AdapterLuid right) {
    return !(left == right);
  }
};

struct SharedCaptureRegistration {
  AdapterLuid luid{};
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t format = 0;
  std::uint64_t generation = 0;
  std::array<std::uint64_t, 3> handles{};
  std::uint32_t slot_count = 0;
};

struct SharedCaptureFrame {
  std::uint32_t slot = 0;
  std::uint64_t generation = 0;
  std::uint64_t sequence = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t format = 0;
  std::uint64_t capture_time_ns = 0;
};

constexpr bool IsValidSharedCaptureRegistration(
    const SharedCaptureRegistration& registration) {
  if (registration.width == 0 || registration.height == 0 ||
      registration.format == 0 || registration.generation == 0 ||
      registration.slot_count == 0 ||
      registration.slot_count > registration.handles.size()) {
    return false;
  }
  for (std::size_t i = 0; i < registration.slot_count; ++i) {
    if (registration.handles[i] == 0) return false;
  }
  return true;
}

constexpr bool IsValidSharedCaptureFrame(
    const SharedCaptureFrame& frame,
    const SharedCaptureRegistration& registration) {
  return IsValidSharedCaptureRegistration(registration) &&
         frame.slot < registration.slot_count &&
         frame.generation == registration.generation && frame.sequence != 0 &&
         frame.width == registration.width && frame.height == registration.height &&
         frame.format == registration.format;
}

struct SharedCaptureMetadata {
  std::uint64_t generation = 0;
  std::uint64_t sequence = 0;

  friend constexpr bool operator==(SharedCaptureMetadata left,
                                   SharedCaptureMetadata right) {
    return left.generation == right.generation &&
           left.sequence == right.sequence;
  }
  friend constexpr bool operator!=(SharedCaptureMetadata left,
                                   SharedCaptureMetadata right) {
    return !(left == right);
  }
};

// Fixed-capacity, nonblocking metadata handoff. Payloads typically contain
// neutral shared-resource descriptors; ownership of GPU resources stays with
// the integrating renderer.
template <typename Payload, std::size_t Capacity>
class SharedCaptureMailbox {
  static_assert(Capacity > 0, "mailbox capacity must be nonzero");

  enum class SlotState : std::uint8_t {
    free,
    writing,
    published,
    acquired,
  };

  struct Slot {
    std::atomic<SlotState> state{SlotState::free};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> sequence{0};
    Payload payload{};
  };

 public:
  class ProducerLease {
   public:
    ProducerLease() = default;
    ProducerLease(const ProducerLease&) = delete;
    ProducerLease& operator=(const ProducerLease&) = delete;

    ProducerLease(ProducerLease&& other) noexcept { MoveFrom(other); }
    ProducerLease& operator=(ProducerLease&& other) noexcept {
      if (this != &other) {
        cancel();
        MoveFrom(other);
      }
      return *this;
    }

    ~ProducerLease() { cancel(); }

    explicit operator bool() const { return owner_ != nullptr; }
    Payload& payload() { return owner_->slots_[index_].payload; }

    bool publish(SharedCaptureMetadata metadata) {
      if (!owner_) return false;
      Slot& slot = owner_->slots_[index_];
      slot.generation.store(metadata.generation, std::memory_order_relaxed);
      slot.sequence.store(metadata.sequence, std::memory_order_relaxed);
      slot.state.store(SlotState::published, std::memory_order_release);
      owner_ = nullptr;
      return true;
    }

    void cancel() {
      if (!owner_) return;
      owner_->slots_[index_].state.store(SlotState::free,
                                         std::memory_order_release);
      owner_ = nullptr;
    }

   private:
    friend class SharedCaptureMailbox;
    ProducerLease(SharedCaptureMailbox* owner, std::size_t index)
        : owner_(owner), index_(index) {}

    void MoveFrom(ProducerLease& other) {
      owner_ = other.owner_;
      index_ = other.index_;
      other.owner_ = nullptr;
    }

    SharedCaptureMailbox* owner_ = nullptr;
    std::size_t index_ = 0;
  };

  class ConsumerLease {
   public:
    ConsumerLease() = default;
    ConsumerLease(const ConsumerLease&) = delete;
    ConsumerLease& operator=(const ConsumerLease&) = delete;

    ConsumerLease(ConsumerLease&& other) noexcept { MoveFrom(other); }
    ConsumerLease& operator=(ConsumerLease&& other) noexcept {
      if (this != &other) {
        release();
        MoveFrom(other);
      }
      return *this;
    }

    ~ConsumerLease() { release(); }

    explicit operator bool() const { return owner_ != nullptr; }
    const Payload& payload() const { return owner_->slots_[index_].payload; }
    SharedCaptureMetadata metadata() const {
      const Slot& slot = owner_->slots_[index_];
      return {slot.generation.load(std::memory_order_relaxed),
              slot.sequence.load(std::memory_order_relaxed)};
    }

    void release() {
      if (!owner_) return;
      owner_->slots_[index_].state.store(SlotState::free,
                                         std::memory_order_release);
      owner_ = nullptr;
    }

   private:
    friend class SharedCaptureMailbox;
    ConsumerLease(SharedCaptureMailbox* owner, std::size_t index)
        : owner_(owner), index_(index) {}

    void MoveFrom(ConsumerLease& other) {
      owner_ = other.owner_;
      index_ = other.index_;
      other.owner_ = nullptr;
    }

    SharedCaptureMailbox* owner_ = nullptr;
    std::size_t index_ = 0;
  };

  SharedCaptureMailbox() = default;
  SharedCaptureMailbox(const SharedCaptureMailbox&) = delete;
  SharedCaptureMailbox& operator=(const SharedCaptureMailbox&) = delete;

  static constexpr std::size_t capacity() { return Capacity; }

  ProducerLease tryReserve() {
    if (resetting_.load(std::memory_order_acquire)) return {};

    for (std::size_t attempt = 0; attempt < Capacity * 2; ++attempt) {
      for (std::size_t i = 0; i < Capacity; ++i) {
        SlotState expected = SlotState::free;
        if (slots_[i].state.compare_exchange_strong(
                expected, SlotState::writing, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
          if (resetting_.load(std::memory_order_acquire)) {
            slots_[i].state.store(SlotState::free, std::memory_order_release);
            return {};
          }
          return ProducerLease(this, i);
        }
      }

      const std::size_t oldest = FindPublished(false);
      if (oldest == Capacity) return {};
      SlotState expected = SlotState::published;
      if (slots_[oldest].state.compare_exchange_strong(
              expected, SlotState::writing, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        if (resetting_.load(std::memory_order_acquire)) {
          slots_[oldest].state.store(SlotState::free,
                                      std::memory_order_release);
          return {};
        }
        return ProducerLease(this, oldest);
      }
    }
    return {};
  }

  ConsumerLease acquireNewest() {
    if (resetting_.load(std::memory_order_acquire)) return {};

    for (std::size_t attempt = 0; attempt < Capacity * 2; ++attempt) {
      const std::size_t newest = FindPublished(true);
      if (newest == Capacity) return {};

      SlotState expected = SlotState::published;
      if (!slots_[newest].state.compare_exchange_strong(
              expected, SlotState::acquired, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        continue;
      }
      if (resetting_.load(std::memory_order_acquire)) {
        slots_[newest].state.store(SlotState::free,
                                    std::memory_order_release);
        return {};
      }

      const SharedCaptureMetadata selected = MetadataAt(newest);
      for (std::size_t i = 0; i < Capacity; ++i) {
        if (i == newest) continue;
        const SharedCaptureMetadata candidate = MetadataAt(i);
        if (IsNewer(candidate, selected)) continue;
        SlotState published = SlotState::published;
        slots_[i].state.compare_exchange_strong(
            published, SlotState::free, std::memory_order_acq_rel,
            std::memory_order_acquire);
      }
      return ConsumerLease(this, newest);
    }
    return {};
  }

  std::size_t drain() {
    std::size_t retired = 0;
    for (Slot& slot : slots_) {
      SlotState expected = SlotState::published;
      if (slot.state.compare_exchange_strong(
              expected, SlotState::free, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        ++retired;
      }
    }
    return retired;
  }

  bool reset() {
    bool expected = false;
    if (!resetting_.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
      return false;
    }

    for (const Slot& slot : slots_) {
      const SlotState state = slot.state.load(std::memory_order_acquire);
      if (state == SlotState::writing || state == SlotState::acquired) {
        resetting_.store(false, std::memory_order_release);
        return false;
      }
    }
    drain();
    for (Slot& slot : slots_) {
      slot.generation.store(0, std::memory_order_relaxed);
      slot.sequence.store(0, std::memory_order_relaxed);
    }
    resetting_.store(false, std::memory_order_release);
    return true;
  }

  std::size_t publishedCount() const { return Count(SlotState::published); }
  std::size_t availableCount() const { return Count(SlotState::free); }
  bool empty() const { return availableCount() == Capacity; }

 private:
  SharedCaptureMetadata MetadataAt(std::size_t index) const {
    return {slots_[index].generation.load(std::memory_order_relaxed),
            slots_[index].sequence.load(std::memory_order_relaxed)};
  }

  static bool IsNewer(SharedCaptureMetadata left,
                      SharedCaptureMetadata right) {
    return left.generation > right.generation ||
           (left.generation == right.generation &&
            left.sequence > right.sequence);
  }

  std::size_t FindPublished(bool newest) const {
    std::size_t selected = Capacity;
    SharedCaptureMetadata selected_metadata{};
    for (std::size_t i = 0; i < Capacity; ++i) {
      if (slots_[i].state.load(std::memory_order_acquire) !=
          SlotState::published) {
        continue;
      }
      const SharedCaptureMetadata candidate = MetadataAt(i);
      if (selected == Capacity ||
          (newest ? IsNewer(candidate, selected_metadata)
                  : IsNewer(selected_metadata, candidate))) {
        selected = i;
        selected_metadata = candidate;
      }
    }
    return selected;
  }

  std::size_t Count(SlotState wanted) const {
    std::size_t count = 0;
    for (const Slot& slot : slots_) {
      if (slot.state.load(std::memory_order_acquire) == wanted) ++count;
    }
    return count;
  }

  std::array<Slot, Capacity> slots_{};
  std::atomic<bool> resetting_{false};
};

}  // namespace mecvr::render
