#pragma once

namespace fusism {
  struct ZFileInflater {
    ZFileInflater(int fd, off64_t offset=0, off64_t size=-1);
    ~ZFileInflater();
    int inflate();
  private:
    int fd_;
    off64_t offset_;
    off64_t size_;
  };
}
