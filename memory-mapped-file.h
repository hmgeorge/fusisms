#pragma once
#include <string>

namespace fusism {

struct MemoryMappedFile {
  MemoryMappedFile(int fd);
  ~MemoryMappedFile();
  MemoryMappedFile(MemoryMappedFile&& other);
  MemoryMappedFile& operator=(MemoryMappedFile&& other);
  bool valid() { return fd_ >= 0; }
  uint8_t operator[](off64_t offset);
  std::string dump(off64_t offset, off64_t len);
  off64_t size() { return size_; }
private:
  MemoryMappedFile(const MemoryMappedFile&);
  MemoryMappedFile& operator=(const MemoryMappedFile&);

  void unmap();
  void remap(off64_t offset);
  void moveFrom(MemoryMappedFile &&other);

  int fd_;
  off64_t size_;
  uint8_t *addr_;
  off64_t map_begin_;
  off64_t map_len_;
};

}
