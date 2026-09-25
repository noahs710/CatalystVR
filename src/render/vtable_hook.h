#pragma once

// Vtable-patch hook utility (M0C1 synthetic observer, T3A).
//
// Why vtable-patching is the permanent hook technique for the DX11 observer
// (not a temporary hack):
//   - No disassembler, no instruction decoding, no prologue analysis: the
//     target address is read directly from the object's vtable slot.
//   - Bounded work: one page-protection change plus one pointer-sized write
//     per slot, both reversed symmetrically on unhook.
//   - Trivially reversible: unhook restores the exact original function
//     pointer, leaving no trace in the process.
// See docs/OBSERVER_M0C1.md for the full rationale and evidence.
#include <cstddef>

namespace mecvr::render {

class VtableHook {
 public:
  VtableHook() = default;
  ~VtableHook() { uninstall(); }

  VtableHook(const VtableHook&) = delete;
  VtableHook& operator=(const VtableHook&) = delete;

  // Swaps vtable slot `index` of `object` for `detour`. Returns false when
  // a hook is already installed on this utility, when either pointer is
  // null, or when the page protection change fails. The caller owns the
  // choice of index; see observer.cc for the documented IDXGISwapChain
  // slot layout.
  bool install(void* object, std::size_t index, void* detour);

  // Restores the original function pointer. Safe to call when no hook is
  // installed (returns true, no work). Returns false only when the page
  // protection change fails.
  bool uninstall();

  bool installed() const { return slot_ != nullptr; }
  void* original() const { return original_; }

 private:
  void** slot_ = nullptr;
  void* original_ = nullptr;
};

}  // namespace mecvr::render
