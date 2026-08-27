#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::string input) : input_(std::move(input)) {}
  explicit HalFile(std::string* output) : output_(output) {}

  explicit operator bool() const { return true; }
  bool operator!() const { return false; }
  int available() const { return static_cast<int>(input_.size() - position_); }
  int read(void* out, size_t size) {
    const size_t count = std::min(size, input_.size() - position_);
    std::memcpy(out, input_.data() + position_, count);
    position_ += count;
    return static_cast<int>(count);
  }
  int read(uint8_t* out, size_t size) { return read(static_cast<void*>(out), size); }
  int write(const uint8_t* data, size_t size) {
    if (output_ == nullptr) return 0;
    output_->append(reinterpret_cast<const char*>(data), size);
    return static_cast<int>(size);
  }
  int write(uint8_t value) { return write(&value, 1); }
  int availableBytes() const { return static_cast<int>(input_.size() - position_); }
  int size() const { return static_cast<int>(input_.size()); }
  size_t position() const { return position_; }
  void close() {}

 private:
  std::string input_;
  std::string* output_ = nullptr;
  size_t position_ = 0;
};

struct HostStorage {
  bool exists(const char* path) const { return files.find(path) != files.end(); }
  bool remove(const char* path) { return files.erase(path) != 0; }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) const {
    const auto found = files.find(path);
    if (found == files.end()) return false;
    file = HalFile(found->second);
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    auto& contents = files[path];
    contents.clear();
    file = HalFile(&contents);
    return true;
  }
  void clear() { files.clear(); }

  std::unordered_map<std::string, std::string> files;
};

inline HostStorage Storage;
