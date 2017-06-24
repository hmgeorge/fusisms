// git parser
//   searches a given sha1 in the pack file (if available)
//   or if there is a obj entry corresponding to the the file
//   supports only the undeltified representation for now
//
//   use: git fetch-pack -k --thin --depth=10 -v git@github.com:torvalds/linux.git HEAD
//   use this program to open a single file instead of cloning the entire git project
// on https://github.com/git/git/blob/master/Documentation/technical/pack-format.txt
// opens the idx file first and searches for the corresponding pack file

// zlib decoder based off http://zlib.net/zlib_how.html
// shoutout to Ben Hoyt's explanation of the pack encoding scheme
// at https://github.com/benhoyt/pygit/blob/master/pygit.py#L441

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

#include <fstream>
#include <functional>
#include <algorithm>
#include <vector>
#include <iomanip>

#include "z-file-inflater.h"
#include "zlib.h"
#include "memory-mapped-file.h"
#include "utils.h"

typedef enum {
  OBJ_NONE,
  OBJ_COMMIT,
  OBJ_TREE,
  OBJ_BLOB,
  OBJ_TAG,
  OBJ_FUTURE,
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
using ZFileInflater = fusism::ZFileInflater;

struct PackIdxReader {
  PackIdxReader(std::string file) : file_name_(file),
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
    case OBJ_BLOB:
      catBlob((*po_it).offset(), (*po_it).size());
      break;
    case OBJ_COMMIT:
      catCommitTree((*po_it).offset(), (*po_it).size());
      break;
    case OBJ_TREE:
      catTree((*po_it).offset(), (*po_it).size());
      break;
    case OBJ_REF_DELTA:
      std::cerr << "ref delta\n";
      break;
    case OBJ_OFS_DELTA:
      catOfsDelta((*po_it).offset(), (*po_it).size());
      break;
    default:
      std::cerr << "unknown type " << (*po_it).type() << "\n";
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

  void catBlob(off64_t offset, off64_t size) {
    MemoryMappedFile out(ZFileInflater(packed_fd_,
				       offset,
				       size).inflate());
    if (!out.valid()) return;
    off64_t cursor = 0;
    while (cursor < size) {
      std::cerr << out[cursor++];
    }
    std::cerr << "\n";
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
    MemoryMappedFile out(ZFileInflater(packed_fd_, offset, size).inflate());
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
    MemoryMappedFile out(ZFileInflater(packed_fd_, offset, size).inflate());
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

  void catOfsDelta(off64_t offset, off64_t size) {
    MemoryMappedFile out(dup(packed_fd_));
    if (!out.valid()) return;

    off64_t cursor = offset;
    uint8_t byte = out[cursor++];
    off64_t delta_offset = (byte&0x7f); // 1SSSSSSS 1SSSSSS 0SSSSSSS
    int32_t sh=4;
    bool cont = byte&0x80;
    while (cont) {
      byte = out[cursor++];
      delta_offset |= ((byte&0x7f)<<sh);
      sh += 7;
      cont = (byte&0x80);
    }
    std::cerr << size << " "
	      << offset << " " << delta_offset << "\n";
    return catBlob(cursor, size);
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
  std::cerr << "pack-reader sha1\n";
  std::cerr << "\t assumes a .git exists in the path to root\n";
}

struct ObjectReader {
  ObjectReader(std::string obj_path) :
    path_(obj_path) {
    fd_ = open(obj_path.c_str(), O_RDONLY);
    struct stat sb;
    if (fstat(fd_, &sb) == 0) {
      size_ = sb.st_size;
    }
  }

  ~ObjectReader() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  void cat() {
    off64_t cursor = 0;
    MemoryMappedFile out(ZFileInflater(fd_).inflate());
    if (!out.valid()) return;

    char type[5]={0};
    for (int i=0; i<4; i++) type[i] = out[i];

    if (!strcmp(type, "tree")) {
      cursor += 4; // skip the size
      cursor += 1; // skip the space
      while (cursor < out.size()) {
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
    } else if (!strcmp(type, "blob")) {
      cursor += 4; // skip the word
      cursor += 1; // skip the space
      cursor += 4; // skip the 32bit size
      while (cursor < out.size()) {
	std::cerr << out[cursor++];
      }
    } else {
      std::cerr << "support for commit tree TBD\n";
    }
  }
private:
  std::string path_;
  int fd_;
  off64_t size_;
};

std::string find_git() {
  char *p = getcwd(NULL, 0);
  std::string s = p;
  free(p);
  while (s != "") {
    auto gp = s+"/.git";
    std::cerr << gp << "\n";
    if (access(gp.c_str(), F_OK) == 0) {
      return gp;
    }
    s = s.substr(0, s.rfind('/'));
  }
  return "";
}

std::string pack_file(std::string git_path) {
  auto pack_info = git_path+"/objects/info/packs";
  if (access(pack_info.c_str(),
	     F_OK|R_OK) < 0) {
    return "";
  }
  std::string line;
  std::ifstream infile(pack_info);
  std::getline(infile, line);
  line = line.substr(line.find(' ')+1, line.find(".pack")-2);
  return git_path+"/objects/pack/"+line+".idx";
}

std::string obj_file(std::string git_path,
		     std::string sha1) {
  auto obj_path = git_path + "/objects/" +
                  sha1.substr(0,2) + "/" +
                  sha1.substr(2);
  std::cerr << obj_path << "\n";
  if (access(obj_path.c_str(), F_OK|R_OK) < 0) {
    return "";
  }
  return obj_path;
}

int main(int argc, char **argv)
{
  if (argc < 2) {
    usage();
    exit(-1);
  }

  std::string git_path = find_git();
  if (git_path == "") {
    std::cerr << "no git found in path /\n";
    return -1;
  }

  std::string pack;
  std::string obj;
  if ((pack=pack_file(git_path)) != "") {
    PackIdxReader reader(pack);
    // reader.list();
    reader.cat(argv[1]);
  } else if ((obj=obj_file(git_path, argv[1])) != "") {
    // see if there is a path of the type
    // git_path/b1b2/b3...b20
    ObjectReader reader(obj);
    reader.cat();
  } else {
    std::cerr << argv[1] << " cannot be looked at\n";
  }
}
