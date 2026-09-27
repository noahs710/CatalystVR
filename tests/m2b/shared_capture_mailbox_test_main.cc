#include "render/shared_capture_mailbox.h"

#include <cstdint>
#include <iostream>
#include <type_traits>

namespace {

struct CaptureDescriptor {
  std::uint32_t texture_index = 0;
  std::uint64_t fence_value = 0;
};

bool Check(bool condition, const char* message) {
  if (!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}

}  // namespace

int main() {
  using Mailbox = mecvr::render::SharedCaptureMailbox<CaptureDescriptor, 3>;
  using mecvr::render::AdapterLuid;
  using mecvr::render::IsValidSharedCaptureFrame;
  using mecvr::render::IsValidSharedCaptureRegistration;
  using Metadata = mecvr::render::SharedCaptureMetadata;
  using mecvr::render::SharedCaptureFrame;
  using mecvr::render::SharedCaptureRegistration;

  static_assert(!std::is_copy_constructible_v<Mailbox>);
  static_assert(!std::is_copy_constructible_v<Mailbox::ProducerLease>);
  static_assert(!std::is_copy_constructible_v<Mailbox::ConsumerLease>);
  static_assert(std::is_trivially_copyable_v<AdapterLuid>);
  static_assert(std::is_trivially_copyable_v<SharedCaptureRegistration>);
  static_assert(std::is_trivially_copyable_v<SharedCaptureFrame>);

  bool ok = true;

  const AdapterLuid adapter{0x89abcdefu, -7};
  ok &= Check(adapter == AdapterLuid{0x89abcdefu, -7} &&
                  adapter != AdapterLuid{0x89abcdefu, 7},
              "neutral adapter LUID equality preserves both halves");
  const SharedCaptureRegistration registration{
      adapter, 2064, 2208, 28, 17, {101, 102, 103}, 3};
  ok &= Check(IsValidSharedCaptureRegistration(registration),
              "complete neutral registration validates");
  auto invalid_registration = registration;
  invalid_registration.slot_count = 4;
  ok &= Check(!IsValidSharedCaptureRegistration(invalid_registration),
              "registration rejects slot count beyond fixed capacity");

  const SharedCaptureFrame frame_record{2, 17, 44, 2064, 2208, 28, 123456789};
  ok &= Check(IsValidSharedCaptureFrame(frame_record, registration),
              "frame matching registration validates");
  auto stale_frame = frame_record;
  stale_frame.generation = 16;
  ok &= Check(!IsValidSharedCaptureFrame(stale_frame, registration),
              "frame from stale registration generation is rejected");

  Mailbox mailbox;

  auto first = mailbox.tryReserve();
  ok &= Check(static_cast<bool>(first), "producer reserves a free slot");
  first.payload() = {11, 101};
  ok &= Check(first.publish(Metadata{4, 1}), "producer publishes reservation");

  auto second = mailbox.tryReserve();
  second.payload() = {12, 102};
  ok &= Check(second.publish(Metadata{4, 2}), "second frame publishes");
  auto third = mailbox.tryReserve();
  third.payload() = {13, 103};
  ok &= Check(third.publish(Metadata{4, 3}), "third frame publishes");

  auto newest = mailbox.acquireNewest();
  ok &= Check(static_cast<bool>(newest), "consumer acquires a published frame");
  ok &= Check(newest.metadata() == Metadata{4, 3},
              "consumer receives newest generation and sequence");
  ok &= Check(newest.payload().texture_index == 13 &&
                  newest.payload().fence_value == 103,
              "consumer receives matching capture descriptor");
  ok &= Check(mailbox.publishedCount() == 0,
              "older published frames retire on newest acquisition");

  // The acquired slot cannot be reused. The other two slots remain available.
  auto replacement_a = mailbox.tryReserve();
  auto replacement_b = mailbox.tryReserve();
  auto blocked = mailbox.tryReserve();
  ok &= Check(replacement_a && replacement_b && !blocked,
              "producer cannot overwrite the acquired consumer slot");
  ok &= Check(newest.payload().texture_index == 13,
              "acquired payload stays unchanged under producer pressure");
  replacement_a.cancel();
  replacement_b.cancel();

  ok &= Check(!mailbox.reset(), "reset refuses while consumer owns a slot");
  newest.release();
  ok &= Check(mailbox.reset(), "reset succeeds after all leases release");
  ok &= Check(mailbox.empty(), "reset leaves mailbox empty");

  auto old_generation = mailbox.tryReserve();
  old_generation.payload() = {20, 200};
  old_generation.publish(Metadata{8, 99});
  auto new_generation = mailbox.tryReserve();
  new_generation.payload() = {21, 201};
  new_generation.publish(Metadata{9, 1});
  auto by_generation = mailbox.acquireNewest();
  ok &= Check(by_generation.metadata() == Metadata{9, 1},
              "generation takes precedence over sequence");
  by_generation.release();

  mecvr::render::SharedCaptureMailbox<CaptureDescriptor, 2> saturated;
  auto saturated_a = saturated.tryReserve();
  saturated_a.payload() = {40, 400};
  saturated_a.publish(Metadata{12, 1});
  auto saturated_b = saturated.tryReserve();
  saturated_b.payload() = {41, 401};
  saturated_b.publish(Metadata{12, 2});
  auto reclaimed = saturated.tryReserve();
  ok &= Check(static_cast<bool>(reclaimed),
              "producer reclaims an older published slot without waiting");
  reclaimed.payload() = {42, 402};
  reclaimed.publish(Metadata{12, 3});
  auto saturated_newest = saturated.acquireNewest();
  ok &= Check(saturated_newest.metadata() == Metadata{12, 3} &&
                  saturated_newest.payload().texture_index == 42,
              "published-slot reclamation preserves the newest frame");
  ok &= Check(saturated.drain() == 0 &&
                  saturated_newest.payload().texture_index == 42,
              "drain never retires an acquired consumer slot");
  saturated_newest.release();

  auto drained_a = mailbox.tryReserve();
  drained_a.publish(Metadata{10, 1});
  auto drained_b = mailbox.tryReserve();
  drained_b.publish(Metadata{10, 2});
  ok &= Check(mailbox.drain() == 2, "drain reports retired published slots");
  ok &= Check(!mailbox.acquireNewest(), "drained mailbox has no frame");

  {
    auto abandoned = mailbox.tryReserve();
    abandoned.payload() = {30, 300};
  }
  ok &= Check(mailbox.availableCount() == Mailbox::capacity(),
              "abandoned producer reservation retires automatically");

  std::cout << (ok ? "shared capture mailbox: PASS\n"
                   : "shared capture mailbox: FAIL\n");
  return ok ? 0 : 1;
}
