// Minimal RGBA PNG writer backed by zlib. No other dependencies.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <zlib.h>

inline bool writePng(const char* path, int w, int h, const uint8_t* rgba) {
  auto put32 = [](std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(v >> 24);
    out.push_back(v >> 16);
    out.push_back(v >> 8);
    out.push_back(v);
  };
  auto chunk = [&](std::vector<uint8_t>& out, const char type[4], const uint8_t* data,
                   size_t len) {
    put32(out, (uint32_t)len);
    out.insert(out.end(), type, type + 4);
    if (data) out.insert(out.end(), data, data + len);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef*)type, 4);
    if (data) crc = crc32(crc, data, (uInt)len);
    put32(out, (uint32_t)crc);
  };

  std::vector<uint8_t> out;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  out.insert(out.end(), sig, sig + 8);

  uint8_t ihdr[13] = {0};
  ihdr[0] = (uint8_t)(w >> 24);
  ihdr[1] = (uint8_t)(w >> 16);
  ihdr[2] = (uint8_t)(w >> 8);
  ihdr[3] = (uint8_t)w;
  ihdr[4] = (uint8_t)(h >> 24);
  ihdr[5] = (uint8_t)(h >> 16);
  ihdr[6] = (uint8_t)(h >> 8);
  ihdr[7] = (uint8_t)h;
  ihdr[8] = 8;   // bit depth
  ihdr[9] = 6;   // color type: RGBA
  ihdr[10] = 0;  // compression
  ihdr[11] = 0;  // filter
  ihdr[12] = 0;  // interlace
  chunk(out, "IHDR", ihdr, 13);

  // Raw scanlines: one filter byte (0 = None) per row, then RGBA data.
  const size_t rowBytes = (size_t)w * 4;
  std::vector<uint8_t> raw((size_t)h * (1 + rowBytes));
  for (int y = 0; y < h; y++) {
    uint8_t* row = &raw[(size_t)y * (1 + rowBytes)];
    row[0] = 0;
    memcpy(row + 1, rgba + (size_t)y * rowBytes, rowBytes);
  }

  uLongf bound = compressBound((uLong)raw.size());
  std::vector<uint8_t> comp(bound);
  if (compress2(comp.data(), &bound, raw.data(), (uLong)raw.size(), 9) != Z_OK) {
    fprintf(stderr, "zlib compression failed\n");
    return false;
  }
  comp.resize(bound);
  chunk(out, "IDAT", comp.data(), comp.size());
  chunk(out, "IEND", nullptr, 0);

  FILE* f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "cannot open %s for writing\n", path);
    return false;
  }
  bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}

inline bool writePng16(const char* path, int w, int h, const uint16_t* rgba16) {
  auto put32 = [](std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(v >> 24); out.push_back(v >> 16); out.push_back(v >> 8); out.push_back(v);
  };
  auto chunk = [&](std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t len) {
    put32(out, (uint32_t)len);
    out.insert(out.end(), type, type + 4);
    if (data) out.insert(out.end(), data, data + len);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef*)type, 4);
    if (data) crc = crc32(crc, data, (uInt)len);
    put32(out, (uint32_t)crc);
  };
  std::vector<uint8_t> out;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  out.insert(out.end(), sig, sig + 8);
  uint8_t ihdr[13] = {0};
  ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16); ihdr[2] = (uint8_t)(w >> 8); ihdr[3] = (uint8_t)w;
  ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16); ihdr[6] = (uint8_t)(h >> 8); ihdr[7] = (uint8_t)h;
  ihdr[8] = 16;  // bit depth
  ihdr[9] = 6;   // color type: RGBA
  chunk(out, "IHDR", ihdr, 13);
  const size_t rowBytes = (size_t)w * 8;  // 4 channels * 2 bytes
  std::vector<uint8_t> raw((size_t)h * (1 + rowBytes));
  for (int y = 0; y < h; y++) {
    uint8_t* row = &raw[(size_t)y * (1 + rowBytes)];
    row[0] = 0;
    const uint16_t* src = rgba16 + (size_t)y * w * 4;
    for (int x = 0; x < w; x++) {
      row[1 + x * 8 + 0] = (uint8_t)(src[x * 4 + 0] >> 8);
      row[1 + x * 8 + 1] = (uint8_t)(src[x * 4 + 0] & 0xFF);
      row[1 + x * 8 + 2] = (uint8_t)(src[x * 4 + 1] >> 8);
      row[1 + x * 8 + 3] = (uint8_t)(src[x * 4 + 1] & 0xFF);
      row[1 + x * 8 + 4] = (uint8_t)(src[x * 4 + 2] >> 8);
      row[1 + x * 8 + 5] = (uint8_t)(src[x * 4 + 2] & 0xFF);
      row[1 + x * 8 + 6] = (uint8_t)(src[x * 4 + 3] >> 8);
      row[1 + x * 8 + 7] = (uint8_t)(src[x * 4 + 3] & 0xFF);
    }
  }
  uLongf bound = compressBound((uLong)raw.size());
  std::vector<uint8_t> comp(bound);
  if (compress2(comp.data(), &bound, raw.data(), (uLong)raw.size(), 9) != Z_OK) return false;
  comp.resize(bound);
  chunk(out, "IDAT", comp.data(), comp.size());
  chunk(out, "IEND", nullptr, 0);
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}
