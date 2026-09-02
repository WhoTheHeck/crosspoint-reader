#pragma once

#include <cstddef>

class String {
 public:
  String() = default;
  explicit String(const char* value) : value_(value ? value : "") {}

  const char* c_str() const { return value_; }
  size_t length() const {
    size_t result = 0;
    while (value_[result] != '\0') ++result;
    return result;
  }

 private:
  const char* value_ = "";
};
