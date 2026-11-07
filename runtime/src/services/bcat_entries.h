#pragma once
#include <algorithm>
#include <string>
#include <vector>
#include <system_error>
#include "bcat_path.h"
#include "bcat_md5.h"
struct BcatEntry { char name[0x20]; uint64_t size; uint8_t digest[0x10]; };
static_assert(sizeof(BcatEntry) == 0x38, "entry size");
static inline int bcat_list_dir(const std::filesystem::path& root, const char* dir, BcatEntry* out, int max) {
  namespace fs = std::filesystem;
  if (!bcat_name_ok(dir)) return -1;
  std::error_code ec;
  fs::path d = root / dir;
  if (fs::symlink_status(d, ec).type() != fs::file_type::directory) return -1;
  std::vector<std::string> names;
  for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->symlink_status(e2).type() != fs::file_type::regular) continue;
    std::string n = it->path().filename().string();
    if (bcat_name_ok(n.c_str())) names.push_back(n);
  }
  std::sort(names.begin(), names.end());
  int c = 0;
  for (auto& n : names) {
    if (c >= max) break;
    BcatEntry& e = out[c]; std::memset(&e, 0, sizeof e);
    std::memcpy(e.name, n.c_str(), n.size());
    std::error_code e2; e.size = (uint64_t)fs::file_size(d / n, e2);
    if (!bcat_file_md5(d / n, e.digest)) continue;
    ++c;
  }
  return c;
}
