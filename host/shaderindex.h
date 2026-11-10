#pragma once
// Shader index: every shader container in the user's own game files, found by unpacking the cooked packages once.
// The shader manifest (which can ship) names shaders by a hash of their microcode; the index turns a hash back into code.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Key of a shader's microcode (host-order words), the same hash build_shader starts from.
uint64_t shader_code_key(const uint32_t* words, size_t n, bool isVs);

// Unpacks every package under cookedDir and writes <outDir>\shader_index.bin (keys and offsets) and
// <outDir>\shader_code.bin (the microcode, host order). progress(done, total) is called per file. False on failure.
bool shaderindex_build(const std::string& cookedDir, const std::string& outDir, const std::function<void(int, int)>& progress);

// Loads the index written above. Afterwards shaderindex_find gives a shader's microcode by key.
bool shaderindex_load(const std::string& outDir);
bool shaderindex_find(uint64_t key, std::vector<uint32_t>& code);
size_t shaderindex_size();
// The index entry of the same kind and length that differs from code in the fewest words (the game patches vertex shaders
// when it binds them): its key and code. False if none is close (more than a quarter of the words differ).
bool shaderindex_closest(const std::vector<uint32_t>& code, bool isVs, uint64_t& key, std::vector<uint32_t>& base);

// Does an XMem LZX stream start at p? Decodes up to `want` bytes; returns how many decoded cleanly (0: not a stream).
size_t xmem_lzx_probe(const uint8_t* p, size_t avail, size_t want, std::vector<uint8_t>& out);

// Unpacks one fully compressed UE3 package (blocks of XMemCompress LZX). False if the file isn't one.
bool ue3_unpack_file(const std::string& path, std::vector<uint8_t>& out);
