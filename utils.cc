#include "utils.h"
#include <stdlib.h>
#include <string>
namespace fusism {
  std::string hexdump(const uint8_t arr[], uint32_t len) {
    std::string s;
    for (uint32_t i=0; i<len; i++) {
      char buf[3]={'.','.','.'};
      sprintf(buf, "%02x", arr[i]);
      s.append(buf);
    }
    return s;
  }
}
