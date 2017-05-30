#include <cassert>
#include <iostream>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdlib.h>

#include "zlib.h"
#include "z-file-inflater.h"

#define ARRAY_SIZE(a) ((sizeof(a))/sizeof((a[0])))

namespace fusism {
  ZFileInflater::ZFileInflater(int fd, off64_t offset,
			       off64_t size) : fd_(fd),
					       offset_(offset),
					       size_(size) { }

  ZFileInflater::~ZFileInflater() { }/*caller has close fd*/

  int ZFileInflater::inflate() {
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

    bool eos = false;
    uint32_t written = 0;
    lseek64(fd_, offset_, SEEK_SET);
    while (!eos) {
      strm.next_out = out;
      strm.avail_out = ARRAY_SIZE(out);
      do {
	if (strm.avail_in == 0) {
	  strm.avail_in = read(fd_, &input, ARRAY_SIZE(input));
	  if (strm.avail_in < 0) {
	    perror("read");
	    close(temp_fd);
	    return -1;
	  }
	  strm.next_in = &input[0];
	}
	ret = ::inflate(&strm, Z_NO_FLUSH);
	assert(ret != Z_STREAM_ERROR); /* state not clobbered */
	switch (ret) {
	case Z_STREAM_END:
	  eos = true;
	case Z_OK:
	  break;
	default:
	  (void)inflateEnd(&strm);
	  std::cerr << ret << " failed to inflate\n";
	  close(temp_fd);
	  return -1;
	}
      } while (!eos && strm.avail_out > 0);
      if (!eos) {
	assert(strm.avail_out == 0);
      }
      written += write(temp_fd, out, ARRAY_SIZE(out)-strm.avail_out);
    }
    (void)inflateEnd(&strm);
    if (size_ != -1 && written != size_) {
      std::cerr << ret << " failed to inflate pack completely\n";
      close(temp_fd);
      return -1;
    }
    // seek to 0 so that users don't have to seek to 0
    // to get the file internals
    lseek64(temp_fd, 0, SEEK_SET);
    return temp_fd;
  }
}
