#include <cassert>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <string.h>
#include <iomanip>
#include <stdlib.h>
#include "utils.h"
#include "memory-mapped-file.h"

namespace fusism {

MemoryMappedFile::MemoryMappedFile(int fd) : fd_(fd) {
    struct stat sb;
    if (fd_ >= 0) {
      fstat(fd_, &sb);
      size_ = sb.st_size;
      remap(0);
    }
}

MemoryMappedFile::~MemoryMappedFile() {
    unmap();
    if (fd_ >= 0) {
      close(fd_);
    }
}

MemoryMappedFile::MemoryMappedFile(MemoryMappedFile&& other) {
  moveFrom(std::forward<MemoryMappedFile>(other));
}

MemoryMappedFile&
MemoryMappedFile::operator=(MemoryMappedFile&& other) {
  moveFrom(std::forward<MemoryMappedFile>(other));
  return *this;
}

uint8_t MemoryMappedFile::operator[](off64_t offset) {
  if (addr_ != NULL) {
    if (offset < map_begin_ || offset >= (map_begin_ + map_len_)) {
      remap(offset);
    }
    return addr_[offset-map_begin_];
  }
  return '\0';
}

std::string MemoryMappedFile::dump(off64_t offset, off64_t len) {
  if (size_ - offset < len) { //offset + len > size_) {
    std::cerr << offset << " " << len << " " << size_ << "\n";
    return "OUTOFBOUNDS";
  }
  std::string s;
  /*
    AAAAAAAXXXXXX|XXXXXXXXXXXX|XXXXBBBBBBBB
  */
  while (len > 0) {
    remap(offset);
    off64_t o = offset - map_begin_;
    uint8_t *addr = addr_ + o;
    off64_t l = std::min(map_len_ - o, len);
    s += hexdump(addr, l);
    offset += map_len_;
    len -= l;
  }
  assert(len == 0);
  return s;
}

void MemoryMappedFile::unmap() {
  if (addr_ != NULL) {
    munmap(addr_, map_len_);
  }
}

void MemoryMappedFile::remap(off64_t offset) {
  assert(offset < size_);
  unmap();
  // remap at page boundary before offset
  long page_sz = sysconf(_SC_PAGESIZE);
  off64_t nearest_page = (offset/page_sz)*page_sz;
  map_begin_ = nearest_page;
  map_len_ = ((size_-map_begin_) > page_sz) ? page_sz :
                                (size_ - nearest_page);
  addr_ = (uint8_t *)mmap(NULL,
			  map_len_,
			  PROT_READ|PROT_WRITE,
			  MAP_PRIVATE,
			  fd_, map_begin_);
  if (addr_ == MAP_FAILED) {
    perror("MMAP FAILED");
    addr_ = nullptr;
  }
}

void MemoryMappedFile::moveFrom(MemoryMappedFile &&other) {
  fd_ = -1;
  size_ = 0;
  addr_ = NULL;
  map_begin_ = 0;
  map_len_ = 0;
  std::swap(fd_, other.fd_);
  std::swap(size_, other.size_);
  std::swap(addr_, other.addr_);
  std::swap(map_begin_, other.map_begin_);
  std::swap(map_len_, other.map_len_);
}

} //namespace fusism
