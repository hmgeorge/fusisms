// git pack file parser based
// on https://github.com/git/git/blob/master/Documentation/technical/pack-format.txt
// opens the idx file first and searches for the corresponding pack file

// zlib decoder based off http://zlib.net/zlib_how.html
// shoutout to Ben Hoyt's explanation of the pack encoding scheme
// at https://github.com/benhoyt/pygit/blob/master/pygit.py#L441


// g++ -std=c++11 git-pack-reader.cc `pkg-config zlib --cflags --libs`
#include <cassert>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include <vector>
#include <iomanip>

#include "zlib.h"
#include "utils.h"

typedef enum {
  OBJ_NONE,
  OBJ_COMMIT,
  OBJ_TREE,
  OBJ_BLOB,
  OBJ_TAG,
  OBJ_OFS_DELTA,
  OBJ_REF_DELTA
} obj_type_t;

std::string typeToStr(obj_type_t t) {
  switch (t) {
  case OBJ_NONE:
    return "none";
  case OBJ_COMMIT:
    return "commit";
  case OBJ_TREE:
    return "tree";
  case OBJ_BLOB:
    return "blob";
  case OBJ_TAG :
    return "tag";
  case OBJ_OFS_DELTA:
    return "ofs delta";
  case OBJ_REF_DELTA:
    return "ref delta";
  default:
    std::cerr << "unknown type\n";
    assert(0);
    break;
  }
  return "";
}

struct PackIdxReader {
  PackIdxReader(char *file) : file_name_(file),
			      addr_ (nullptr),
			      len_(0),
			      fd_(-1),
			      cursor_(0),
			      init_check_(false),
			      packed_fd_(-1) {
    int fd = open(file_name_.c_str(), O_RDONLY);
    if (fd < 0) {
      perror("open");
      return;
    }
    struct stat sb;
    if (fstat(fd, &sb) < 0) {
      perror("stat");
      return;
    }
    uint8_t *addr = (uint8_t *)mmap(NULL,
				    sb.st_size,
				    PROT_READ,
				    MAP_PRIVATE,
				    fd, 0);
    if (addr == MAP_FAILED) {
      perror("mmap");
      return;
    }

    addr_ = addr;
    fd_ = fd;
    len_ = sb.st_size;

    char magic[5] = {0};
    readBytes(&magic, 4);
    if (strcmp(magic, "\377tOc")) {
      std::cerr << "not an idx file\n";
      return;
    }

    uint32_t version;
    readBytes(&version, sizeof(version));
    version = ntohl(version);
    if (version != 2) {
      std::cerr << "version > 2 unsupported" << "\n";
      return;
    }

    if (setupPackedFd() < 0) {
      std::cerr << "no companion pack file found\n";
      return;
    }

    populate();
  }

  int list() {
    if (!init_check_) {
      std::cerr << "init_check failed" << "\n";
      return -1;
    }

    for (auto &po : pack_objects_) {
      lseek64(packed_fd_, po.offset(), SEEK_SET);
      uint8_t byte;
      read(packed_fd_, &byte, 1);
      int type = (byte>>4)&0x7;
      uint64_t obj_size = (byte&0xf); // 0TTTSSSS 1SSSSSS 0SSSSSSS
      int32_t sh=4;
      bool cont = byte&0x80;
      while (cont) {
	read(packed_fd_, &byte, 1);
	obj_size |= ((byte&0x7f)<<sh);
	sh += 7;
	cont = (byte & 0x80);
      }

      std::cerr << po.sha1()
		<< " "
		<< std::setw(8) << obj_size
		<< " "
		<< typeToStr((obj_type_t)type)
		<< "\n";
    }
  }

  ~PackIdxReader( ) {
    if (addr_) {
      munmap(addr_, len_);
    }
    if (fd_ != -1) {
      close(fd_);
    }
    if (packed_fd_ != -1) {
      close(packed_fd_);
    }
  }
  
private:
  struct PackObject {
    PackObject(std::string s, off64_t offset) : sha1_(s), offset_(offset) { }
    std::string sha1() { return sha1_; }
    off64_t offset() { return offset_; }
    void setOffset(off64_t o) { offset_ = o; }
  private:
    std::string sha1_;
    off64_t offset_;
  };

  ssize_t populate() {
    // skip over to end of fan out table
    // last element has total num of objects
    forward(255*sizeof(uint32_t));

    uint32_t entries;
    readBytes(&entries, sizeof(entries));
    entries = ntohl(entries);

    std::cerr << entries << "\n";
    for (uint32_t i=0; i<entries; ++i) {
      uint8_t sha1[21] = {0};
      readBytes(&sha1[0], 20);
#if DEBUG
      std::cerr << hexdump(sha1, 20) << "\n";
#endif
      pack_objects_.emplace_back(hexdump(sha1, 20), -1);
    }

    // skip over the table of 4-byte crc32 values
    forward(entries*sizeof(uint32_t));

    for (uint32_t i=0; i<entries; ++i) {
      uint32_t offset;
      readBytes(&offset, sizeof(offset));
      offset = ntohl(offset);
      if (offset & 0x80000000) {
	// this is an index into the next table, ignore for now
	std::cerr << "ignore large file offsets\n";
	continue;
      }
      pack_objects_[i].setOffset(offset);
#if DEBUG
      std::cerr << offset <<"\n";
#endif
    }

    std::cerr << "file looks good\n";
    init_check_ = true;
  }

  int setupPackedFd() {
    int pos = file_name_.rfind(".idx");
    if (pos == -1) {
      std::cerr << "no idx file suffix found\n";
      return -1;
    }

    std::string pack_str = file_name_.substr(0, pos) + ".pack";
    packed_fd_ = open(pack_str.c_str(), O_RDONLY);
    if (packed_fd_ < 0) {
      std::cerr << "couldn't find pack string\n";
      return -1;
    }
    return 0;
  }

  ssize_t readBytes(void *bytes, off64_t num) {
    assert(len_ - cursor_ > num);
    memcpy(bytes, addr_ + cursor_, num);
    cursor_ += num;
    return num;
  }

  ssize_t seekTo(off64_t off) {
    assert(0 <= off && off < len_);
    cursor_ = off;
  }

  ssize_t rewind(off64_t num) {
    assert(cursor_ >= num);
    cursor_ -= num;
    return 0;
  }

  ssize_t forward(off64_t num) {
    assert(len_ - cursor_ > num);
    cursor_ += num;
    return 0;
  }

private:
  std::string file_name_;
  off64_t cursor_;
  int fd_;
  off64_t len_;
  uint8_t *addr_;
  bool init_check_;
  int packed_fd_;
  std::vector<PackObject> pack_objects_;
};

// read the idx file.
// use it to index into the .pack file.
int main(int argc, char **argv)
{
  PackIdxReader reader(argv[1]);
  reader.list();
}
