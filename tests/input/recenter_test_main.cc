#include "input/recenter.h"

#include <cstdlib>
#include <iostream>

int main() {
  mecvr::input::RecenterLatch latch;
  if (latch.update(false, false)) return 7;
  if (latch.update(false, false)) return 1;
  if (!latch.update(true, true)) return 2;
  if (latch.update(true, true)) return 3;
  if (latch.update(true, false)) return 4;
  if (!latch.update(true, true)) return 5;
  latch.reset();
  if (!latch.update(true, true)) return 6;
  std::cout << "recenter latch ok\n";
  return 0;
}
