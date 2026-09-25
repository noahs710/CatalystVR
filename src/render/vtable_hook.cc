#include "render/vtable_hook.h"

#include <windows.h>

namespace mecvr::render {

bool VtableHook::install(void* object, std::size_t index, void* detour) {
  if (slot_ != nullptr || object == nullptr || detour == nullptr) {
    return false;
  }
  void** vtable = *reinterpret_cast<void***>(object);
  if (vtable == nullptr) {
    return false;
  }
  void** slot = &vtable[index];
  DWORD old_protect = 0;
  if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect) == 0) {
    return false;
  }
  original_ = *slot;
  *slot = detour;
  DWORD ignored = 0;
  VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
  slot_ = slot;
  return true;
}

bool VtableHook::uninstall() {
  if (slot_ == nullptr) {
    return true;
  }
  DWORD old_protect = 0;
  if (VirtualProtect(slot_, sizeof(void*), PAGE_READWRITE, &old_protect) ==
      0) {
    return false;
  }
  *slot_ = original_;
  DWORD ignored = 0;
  VirtualProtect(slot_, sizeof(void*), old_protect, &ignored);
  slot_ = nullptr;
  original_ = nullptr;
  return true;
}

}  // namespace mecvr::render
