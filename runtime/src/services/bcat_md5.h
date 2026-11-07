#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
struct BcatMd5Ctx { uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476; };
static inline void bcat_md5_block(BcatMd5Ctx& s, const uint8_t* p) {
  static const uint32_t K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
  static const int R[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
  uint32_t M[16];
  for (int i = 0; i < 16; ++i) M[i] = (uint32_t)p[4*i] | ((uint32_t)p[4*i+1] << 8) | ((uint32_t)p[4*i+2] << 16) | ((uint32_t)p[4*i+3] << 24);
  uint32_t A = s.a, B = s.b, C = s.c, D = s.d;
  for (int i = 0; i < 64; ++i) {
    uint32_t F; int g;
    if (i < 16) { F = (B & C) | (~B & D); g = i; }
    else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) & 15; }
    else if (i < 48) { F = B ^ C ^ D; g = (3*i + 5) & 15; }
    else { F = C ^ (B | ~D); g = (7*i) & 15; }
    F = F + A + K[i] + M[g];
    A = D; D = C; C = B;
    B = B + ((F << R[i]) | (F >> (32 - R[i])));
  }
  s.a += A; s.b += B; s.c += C; s.d += D;
}
struct BcatMd5Stream {
  BcatMd5Ctx s; uint8_t buf[64]; size_t n = 0; uint64_t total = 0;
  void update(const uint8_t* p, size_t len) {
    total += len;
    while (len) {
      size_t k = 64 - n; if (k > len) k = len;
      std::memcpy(buf + n, p, k); n += k; p += k; len -= k;
      if (n == 64) { bcat_md5_block(s, buf); n = 0; }
    }
  }
  void finish(uint8_t out[16]) {
    uint64_t bits = total * 8;
    uint8_t pad = 0x80; update(&pad, 1);
    uint8_t z = 0; while (n != 56) update(&z, 1);
    uint8_t l[8]; for (int i = 0; i < 8; ++i) l[i] = (uint8_t)(bits >> (8*i));
    update(l, 8);
    uint32_t v[4] = {s.a, s.b, s.c, s.d};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[4*i+j] = (uint8_t)(v[i] >> (8*j));
  }
};
static inline void bcat_md5(const uint8_t* d, size_t n, uint8_t out[16]) { BcatMd5Stream m; m.update(d, n); m.finish(out); }
static inline bool bcat_file_md5(const std::filesystem::path& p, uint8_t out[16]) {
  std::ifstream f(p, std::ios::binary); if (!f) return false;
  BcatMd5Stream m; char b[4096];
  while (f.read(b, sizeof b) || f.gcount() > 0) m.update((const uint8_t*)b, (size_t)f.gcount());
  m.finish(out); return true;
}
