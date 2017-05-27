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

#include <string>
#include <memory>

std::string hexdump(uint8_t arr[], uint32_t len) {
  std::string s;
  for (uint32_t i=0; i<len; i++) {
    char buf[3]={'.','.','.'};
    sprintf(buf, "%02x", arr[i]);
    s.append(buf);
  }
  return s;
}

// based off https://github.com/git/git/blob/master/Documentation/technical/index-format.txt
// supports version 2 for now.
void git_index_read(unsigned char *addr, int len) {
  int cursor = 0;
  char word[5]={0};
  memcpy(&word[0], addr + cursor, 4);
  cursor += 4;

  std::cerr << word << "\n"; // DIRC

  uint32_t version=0;
  memcpy(&version, addr + cursor, 4);
  cursor += 4;
  version = ntohl(version);

  std::cerr << version << "\n"; // version

  if (version != 2) {
    std::cerr << "version > 2 parsing support missing\n";
    return;
  }

  int32_t num_entries=0;
  memcpy(&num_entries, addr + cursor, 4);
  cursor += 4;
  num_entries = ntohl(num_entries);

  std::cerr << num_entries << "\n"; // version

  int i;
  for (i=0; i<num_entries; i++) {
    cursor += 4; // skip over ctime seconds
    cursor += 4; // skip over ctime nanoseconds fractions
    cursor += 4; // skip over mtime seconds
    cursor += 4; // skip over mtime nano seconds
    cursor += 4; // skip over dev no

    uint32_t inode;
    memcpy(&inode, addr + cursor, 4);
    cursor += 4;
    inode = ntohl(inode);

    uint32_t mode;
    memcpy(&mode, addr + cursor, 4);
    cursor += 4;
    mode = ntohl(mode);

    std::string type;
    switch ((mode>>12)&0xf) {
    case 8:
      type = "file";
      break;
    case 10:
      type = "link";
      break;
    case 12:
      type = "gitlink";
      break;
    default:
      printf("mode 0x%x\n", mode);
      type = "unknown";
      break;
    }

    int perm = (mode>>16)&0x1ff;

    cursor += 4; // skip over uid
    cursor += 4; // skip over gid

    uint32_t file_size;
    memcpy(&file_size, addr + cursor, 4);
    cursor += 4;
    file_size = ntohl(file_size);

    uint8_t sha1[21]={0};
    memcpy(&sha1, addr + cursor, 20);
    cursor += 20;

    uint16_t flags;
    memcpy(&flags, addr + cursor, 2);
    cursor += 2;
    flags = ntohs(flags);

    uint32_t file_len = flags&0xfff;
    char *file_path = (char *)calloc(1, file_len+1);
    memcpy(file_path, addr + cursor, file_len);
    file_path[file_len]='\0';
    cursor += file_len;
    std::cerr << type << " "
	      << hexdump(sha1, sizeof(sha1)-1) << " "
      //	      << inode <<  " "
	      << file_path << " "
	      << file_size << "\n";
    free(file_path);

    while (addr[cursor]=='\0') {
      cursor++;
    }
  }

  char ext[5]={0};
  memcpy(&ext[0], addr + cursor, 4);
  cursor += 4;
  std::cerr << ext << "\n";

  if (strcmp(ext, "TREE"))
    return;

  uint32_t ext_size = 0;
  memcpy(&ext_size, addr + cursor, 4);
  cursor += 4;
  ext_size = ntohl(ext_size);

  std::cerr << ext_size << "\n";

  if (cursor + ext_size >= len)
    return; //bad index

  uint32_t ext_i = 0;
  while (ext_i < ext_size) {
    int j=0;
    while (addr[cursor + j] != '\0') j++; // i must be < ext_size
    char *path = (char *)calloc(1, j+1);
    memcpy(path, addr + cursor, j);
    cursor += j+1; // skip over the \0 as well
    ext_i += j+1;

    j=0;
    while (addr[cursor + j] != ' ') j++; // skip over number of entries
    char *entries = (char *)calloc(1, j+1);
    memcpy(entries, addr + cursor, j);
    int num_entries = atoi(entries);
    cursor += j+1; // skip over the space as well before parsing next
    ext_i += j+1;

    j=0;
    while (addr[cursor + j] != '\n') j++; // skip over n of subtrees
    char *subtrees = (char *)calloc(1, j+1);
    memcpy(subtrees, addr + cursor, j);
    int num_sub_tress = atoi(subtrees);
    cursor += j+1;
    ext_i += j+1;

    uint8_t sha1[21] = {0};
    memcpy(&sha1[0], addr + cursor, 20);
    cursor += 20;
    ext_i += 20;
    std::cerr << num_entries << " " << num_sub_tress <<
      " (" << path << ") " << hexdump(sha1, 20) << "\n";
    free(path);
    free(entries);
    free(subtrees);
  }
}

int main(int argc, char **argv)
{
  int fd = open(argv[1], O_RDONLY);
  if (fd < 0) {
    perror("open");
    return -1;
  }
  struct stat sb;
  if (fstat(fd, &sb) < 0) {
    perror("stat");
    return -1;
  }

  unsigned char *addr = (unsigned char *)mmap(NULL,
			     sb.st_size,
			     PROT_READ,
			     MAP_PRIVATE,
			     fd, 0);
  if (addr == MAP_FAILED) {
    perror("mmap");
    return -1;
  }

  git_index_read(addr, sb.st_size);
  munmap(addr, sb.st_size);
  close(fd);
}
