// git pack file parser based
// on https://github.com/git/git/blob/master/Documentation/technical/pack-format.txt
// opens the idx file first and searches for the corresponding pack file

// zlib decoder based off http://zlib.net/zlib_how.html
// shoutout to Ben Hoyt's explanation of the pack encoding scheme
// at https://github.com/benhoyt/pygit/blob/master/pygit.py#L441


// g++ -std=c++11 git-pack-reader.cc `pkg-config zlib --cflags --libs` -o pack-reader
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

#include <functional>
#include <algorithm>
#include <vector>
#include <iomanip>

#include "zlib.h"
#include "memory-mapped-file.h"
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

using MemoryMappedFile = fusism::MemoryMappedFile;

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

  void cat(const char *sha1) {
    if (!init_check_) {
      std::cerr << "init_check failed" << "\n";
      return;
    }

    auto po_it = std::find_if(pack_objects_.begin(),
			      pack_objects_.end(),
			      [&](PackObject &po) -> bool {
				return po.sha1() == sha1;
			      });
    if (po_it == pack_objects_.end()) {
      std::cerr << fusism::hexdump(reinterpret_cast<const uint8_t *>(sha1),
				   strlen(sha1))  << " not found\n";
      return;
    }
    switch ((*po_it).type()) {
    case OBJ_COMMIT:
      catCommitTree((*po_it).offset(), (*po_it).size());
      break;
    case OBJ_TREE:
      catTree((*po_it).offset(), (*po_it).size());
      break;
    default:
      break;
    }
  }

  int list() {
    if (!init_check_) {
      std::cerr << "init_check failed" << "\n";
      return -1;
    }

    for (auto &po : pack_objects_) {
      std::cerr << po.sha1()
		<< " "
		<< std::setw(8) << po.size()
		<< " "
		<< typeToStr(po.type())
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
    PackObject(std::string sha1,
	       obj_type_t t,
	       off64_t offset,
	       off64_t size) : sha1_(sha1),
			       type_(t),
			       offset_(offset),
			       size_(size) { }
    std::string sha1() { return sha1_; }
    void update(std::function<uint8_t(void) > br) {
      uint8_t byte = br();
      type_ = (obj_type_t)((byte>>4)&0x7);
      size_ = (byte&0xf); // 0TTTSSSS 1SSSSSS 0SSSSSSS
      int32_t sh=4;
      bool cont = byte&0x80;
      while (cont) {
	byte = br();
	size_ |= ((byte&0x7f)<<sh);
	sh += 7;
	cont = (byte & 0x80);
      }
    }
    off64_t offset() { return offset_; }
    obj_type_t type() { return type_; }
    off64_t size() { return size_; }
    void setOffset(off64_t o) { offset_ = o; }
    void setSize(off64_t size) { size_ = size; }
    void setType(obj_type_t t) { type_ = t; }

  private:
    std::string sha1_;
    off64_t offset_;
    off64_t size_;
    obj_type_t type_;
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
      std::cerr << fusism::hexdump(sha1, 20) << "\n";
#endif
      pack_objects_.emplace_back(fusism::hexdump(sha1, 20),
				 OBJ_NONE,
				 0,
				 -1);
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
      if (offset == 246) {
	std::cerr << "add entry at offset " << offset << "\n";
      }
      lseek64(packed_fd_, offset, SEEK_SET);
      pack_objects_[i].update([=](void) -> uint8_t {
	uint8_t byte;
	read(packed_fd_, &byte, 1);
	return byte;
	});
      pack_objects_[i].setOffset(lseek64(packed_fd_, 0, SEEK_CUR));
#if DEBUG
      std::cerr << offset <<"\n";
#endif
    }

    std::cerr << "file looks good\n";
    init_check_ = true;
  }

  // offset in the pack file and uncompressed size
  // as per the spec.
  // after inflate, the format for the tree seems to be
  // 6 bytes permission (e.g 100644)
  // 1 byte space (0x20)
  // NUL terminated path name (e.g 616c6c6f6376312e6300)
  // 20 byte sha1 (e.g c4d5514e3a9fe3ee04d3d12d89eecf5a8eac3ebb)
  // perhaps type of an object can be printed by checking the sha1
  // with the pack objects
  void catTree(off64_t offset, off64_t size) {
    MemoryMappedFile out(zInflate(offset, size));
    if (!out.valid()) return;
    off64_t cursor = 0;
    while (cursor < size) {
      // skip permission
      while (out[cursor] != ' ') ++cursor;
      // skip space as well
      cursor += 1;
      while (out[cursor] != '\0') {
	std::cerr << out[cursor];
	cursor += 1;
      }
      cursor += 1; // skip NUL as well
      std::cerr << " ";
      std::cerr << out.dump(cursor, 20) << "\n";
      cursor += 20;
    }
  }

  void catCommitTree(off64_t offset, off64_t size) {
    MemoryMappedFile out(zInflate(offset, size));
    if (!out.valid()) return;
#if DEBUG
    std::cerr << out.dump(0, size) << "\n";
#endif
    off64_t cursor = 0;
    cursor += 4; // skip over TREE

    cursor += 1; // skip over space

    // next 40 chars contains the string
    // representation of the 20 byte sha1
    char sha1[41]={0};
    for (uint32_t i=0; i<40; ++i) {
      sha1[i] = out[cursor];
      cursor+=1;
    }

    return cat(&sha1[0]);
  }

#define ARRAY_SIZE(a) ((sizeof(a))/sizeof((a[0])))
  // returns fd to a tmp file
  int zInflate(off64_t offset, off64_t size) {
    uint8_t input[1024];
    uint8_t out[17];
    char tmplate[16]={0};
    snprintf(tmplate, sizeof(tmplate), "%s",
	     "/tmp/catXXXXXX");
    z_stream strm;
    int ret;

    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    if (inflateInit(&strm) != Z_OK) {
      std::cerr << "failed to initiaze z_stream\n";
      return -1;
    }

    int temp_fd = mkstemp(tmplate);
    if (temp_fd < 0) {
      perror("mkstemp");
      return -1;
    }

    lseek64(packed_fd_, offset, SEEK_SET);
    bool eos = false;
    uint32_t written = 0;
    while (!eos) {
      strm.next_out = out;
      strm.avail_out = ARRAY_SIZE(out);
      do {
	if (strm.avail_in == 0) {
	  strm.avail_in = read(packed_fd_, &input, ARRAY_SIZE(input));
	  if (strm.avail_in < 0) {
	    perror("read");
	    return -1;
	  }
	  strm.next_in = &input[0];
	}
	ret = inflate(&strm, Z_NO_FLUSH);
	assert(ret != Z_STREAM_ERROR); /* state not clobbered */
	switch (ret) {
	case Z_STREAM_END:
	  eos = true;
	case Z_OK:
	  break;
	default:
	  (void)inflateEnd(&strm);
	  std::cerr << ret << " failed to inflate pack\n";
	  return -1;
	}
      } while (!eos && strm.avail_out > 0);
      if (!eos) {
	assert(strm.avail_out == 0);
      }
      written += write(temp_fd, out, ARRAY_SIZE(out)-strm.avail_out);
    }
    (void)inflateEnd(&strm);
    if (written != size) {
      std::cerr << ret << " failed to inflate pack completely\n";
      return -1;
    }
    return temp_fd;
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

void usage() {
  std::cerr << "pack-reader /path/to/idx/file sha1\n";
}

// read the idx file.
// use it to index into the .pack file.
int main(int argc, char **argv)
{
  if (argc < 3) {
    usage();
    exit(-1);
  }
  PackIdxReader reader(argv[1]);
  // reader.list();
  reader.cat(argv[2]);
}
