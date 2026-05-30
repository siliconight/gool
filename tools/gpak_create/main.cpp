// Copyright 2026 Brannen Graves
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing permissions
// and limitations under the License.

// tools/gpak_create/main.cpp
//
// Build a .gpak from a list of input files. Usage:
//
//   gpak_create out.gpak file1.wav file2.ogg path/to/file3.wav
//   gpak_create out.gpak --base assets/ assets/sfx/foo.wav assets/music/bar.ogg
//
// The first form stores entries by their raw path argument. The
// second form strips a leading prefix (`--base`), so the in-pack
// names are relative to the prefix and match what your sound-bank
// JSON would reference.

#include "audio_engine/gpak.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto sz = in.tellg();
    if (sz < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (!out.empty()) {
        in.read(reinterpret_cast<char*>(out.data()),
                 static_cast<std::streamsize>(out.size()));
    }
    return static_cast<bool>(in);
}

void PrintUsage(const char* exe) {
    std::fprintf(stderr,
                  "usage: %s <out.gpak> [--base <prefix>] <file> [<file>...]\n"
                  "\n"
                  "Build a Gpak from a list of input files. With --base,\n"
                  "the prefix is stripped from each input path before it's\n"
                  "stored as the in-pack entry name.\n",
                  exe);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { PrintUsage(argv[0]); return 1; }

    const std::string outPath = argv[1];
    std::string base;
    int argi = 2;
    if (argi < argc && std::strcmp(argv[argi], "--base") == 0) {
        if (argi + 1 >= argc) { PrintUsage(argv[0]); return 1; }
        base = argv[argi + 1];
        if (!base.empty() && base.back() != '/' && base.back() != '\\') {
            base.push_back('/');
        }
        argi += 2;
    }
    if (argi >= argc) { PrintUsage(argv[0]); return 1; }

    audio::PakWriter writer;
    for (int i = argi; i < argc; ++i) {
        const std::string path = argv[i];
        std::vector<uint8_t> bytes;
        if (!ReadFile(path, bytes)) {
            std::fprintf(stderr, "failed to read %s\n", path.c_str());
            return 1;
        }
        std::string name = path;
        if (!base.empty() &&
             name.size() >= base.size() &&
             name.compare(0, base.size(), base) == 0) {
            name = name.substr(base.size());
        }
        writer.AddEntry(std::move(name), std::move(bytes));
    }

    if (!writer.Write(outPath)) {
        std::fprintf(stderr, "write failed: %s\n",
                      writer.errorMessage().c_str());
        return 1;
    }
    std::fprintf(stderr, "wrote %s with %d entries\n",
                  outPath.c_str(), argc - argi);
    return 0;
}
