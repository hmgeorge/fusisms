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

void fill(uint32_t &w, uint8_t *addr, int &cursor) {
  memcpy(&w, addr + cursor, 4);
  w = ntohl(w);
  cursor += 4;
}

void fill(char *arr, size_t len, uint8_t *addr, int &cursor){
  memcpy(arr, addr+cursor, len);
  cursor += len;
}

void fill(uint16_t &s, uint8_t *addr, int &cursor) {
  memcpy(&s, addr + cursor, sizeof(uint16_t));
  s = ntohs(s);
  cursor += 2;
}

// based off https://github.com/git/git/blob/master/Documentation/technical/index-format.txt
void git_index_read(unsigned char *addr, int len) {
  int cursor = 0;
  char word[4];
  memcpy(&word[0], addr + cursor, 4);
  cursor += 4;

  printf("%s\n", word); // DIRC

  uint32_t version=0;
  memcpy(&version, addr + cursor, 4);
  cursor += 4;

  printf("%u\n", ntohl(version)); // version

  int32_t num_entries=0;
  memcpy(&num_entries, addr + cursor, 4);
  cursor += 4;
  num_entries = ntohl(num_entries);

  printf("%u\n", num_entries); // version

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

    char sha1[21]={0};
    memcpy(&sha1, addr + cursor, 20);
    cursor += 20;

    uint16_t flags;
    memcpy(&flags, addr + cursor, 2);
    cursor += 2;
    flags = ntohs(flags);

    int32_t file_len = flags&0xfff;
    char *file_path = (char *)calloc(1, file_len+1);
    memcpy(file_path, addr + cursor, file_len);
    cursor += file_len;
    fprintf(stderr, "%s %u %s file_len %d\n", type.c_str(), inode, file_path, file_len);
    free(file_path);

    while (addr[cursor]=='\0') {
      cursor++;
    }
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