#pragma once

#include <string>

class String {
 public:
  String() = default;
  explicit String(const char* value) : value_(value ? value : "") {}
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }

 private:
  std::string value_;
};
