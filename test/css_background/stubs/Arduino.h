#pragma once

#include <cstddef>

struct HostEsp {
  unsigned getFreeHeap() const { return 1024 * 1024; }
};

inline HostEsp ESP;
