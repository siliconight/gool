// Gpak format implementation: reader, writer, sound-bank loader adapter.
//
// Format is little-endian binary; we read/write via memcpy on platform-
// native unsigned types, which is correct on every platform we target
// (x86, ARM, RISC-V — all little-endian for the audio engine's
// supported set). If you port to a big-endian platform, byteswap the
// header and index fields explicitly.

#include "audio_engine/gpak.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <utility>

namespace audio {

namespace {

constexpr size_t kHeaderSize     = 24;
constexpr uint32_t kMaxNameBytes = 4096;
constexpr size_t kMaxEntries     = 1u << 24;   // 16 M entries
constexpr uint64_t kMaxFileSize  = 1ull << 36; // 64 GB

bool ReadHeader(const uint8_t* data, size_t size, GpakHeader& out, std::string& err) {
    if (size < kHeaderSize) {
        err = "file too small to contain a Gpak header";
        return false;
    }
    std::memcpy(&out, data, kHeaderSize);
    if (out.magic != kGpakMagic) {
        err = "wrong magic; not a Gpak file";
        return false;
    }
    if (out.version != kGpakVersion) {
        err = "unsupported Gpak version " + std::to_string(out.version);
        return false;
    }
    if (out.entryCount > kMaxEntries) {
        err = "entry count exceeds maximum (" +
              std::to_string(kMaxEntries) + ")";
        return false;
    }
    if (out.indexOffset < kHeaderSize || out.indexOffset > size) {
        err = "index offset out of range";
        return false;
    }
    // An empty pack has indexOffset == size (no data, no index).
    if (out.entryCount == 0 && out.indexOffset != size) {
        err = "empty pack but indexOffset != end of file";
        return false;
    }
    return true;
}

bool ReadIndex(const uint8_t* data, size_t size,
                const GpakHeader& header,
                std::vector<GpakEntry>& outEntries,
                std::string& err) {
    outEntries.clear();
    outEntries.reserve(header.entryCount);
    size_t pos = static_cast<size_t>(header.indexOffset);

    for (uint32_t i = 0; i < header.entryCount; ++i) {
        if (pos + 4 + 8 + 8 > size) {
            err = "index entry " + std::to_string(i) +
                  " runs past end of file";
            return false;
        }
        uint32_t nameLen;
        uint64_t off, sz;
        std::memcpy(&nameLen, data + pos,            4); pos += 4;
        std::memcpy(&off,     data + pos,            8); pos += 8;
        std::memcpy(&sz,      data + pos,            8); pos += 8;

        if (nameLen == 0 || nameLen > kMaxNameBytes) {
            err = "index entry " + std::to_string(i) +
                  " has invalid name length " + std::to_string(nameLen);
            return false;
        }
        if (pos + nameLen > size) {
            err = "index entry " + std::to_string(i) +
                  " name runs past end of file";
            return false;
        }
        if (off < kHeaderSize ||
            sz > kMaxFileSize ||
            off + sz > header.indexOffset) {
            err = "index entry " + std::to_string(i) +
                  " data range out of bounds";
            return false;
        }
        GpakEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(data + pos), nameLen);
        entry.dataOffset = off;
        entry.dataSize   = sz;
        outEntries.push_back(std::move(entry));
        pos += nameLen;
    }
    return true;
}

} // namespace (anon)

// ---- PakReader -----------------------------------------------------

struct PakReader::Impl {
    std::vector<uint8_t>                          owned;       // when Open(path)
    const uint8_t*                                 dataPtr = nullptr;
    size_t                                         dataSize = 0;
    GpakHeader                                     header{};
    std::vector<GpakEntry>                         entries;
    std::unordered_map<std::string, size_t>        nameToIndex;
    std::string                                    error;
};

PakReader::PakReader() : impl_(std::make_unique<Impl>()) {}
PakReader::~PakReader() = default;
PakReader::PakReader(PakReader&&) noexcept = default;
PakReader& PakReader::operator=(PakReader&&) noexcept = default;

bool PakReader::Open(std::string_view path) {
    impl_->owned.clear();
    impl_->entries.clear();
    impl_->nameToIndex.clear();
    impl_->error.clear();

    std::ifstream in((std::string(path)), std::ios::binary);
    if (!in) {
        impl_->error = "could not open " + std::string(path);
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto sz = in.tellg();
    if (sz < 0) {
        impl_->error = "could not determine file size";
        return false;
    }
    in.seekg(0, std::ios::beg);
    impl_->owned.resize(static_cast<size_t>(sz));
    if (!impl_->owned.empty()) {
        in.read(reinterpret_cast<char*>(impl_->owned.data()),
                 static_cast<std::streamsize>(impl_->owned.size()));
    }
    if (!in) {
        impl_->error = "read of " + std::string(path) + " failed mid-stream";
        return false;
    }
    return OpenInMemory(impl_->owned.data(), impl_->owned.size());
}

bool PakReader::OpenInMemory(const uint8_t* data, size_t size) {
    impl_->entries.clear();
    impl_->nameToIndex.clear();
    impl_->error.clear();
    impl_->dataPtr  = data;
    impl_->dataSize = size;

    if (!ReadHeader(data, size, impl_->header, impl_->error))           return false;
    if (!ReadIndex(data, size, impl_->header, impl_->entries, impl_->error)) return false;

    impl_->nameToIndex.reserve(impl_->entries.size());
    for (size_t i = 0; i < impl_->entries.size(); ++i) {
        const auto& e = impl_->entries[i];
        if (!impl_->nameToIndex.emplace(e.name, i).second) {
            impl_->error = "duplicate entry name '" + e.name + "' in pack";
            return false;
        }
    }
    return true;
}

bool PakReader::Read(std::string_view name, std::vector<uint8_t>& outBytes) const {
    const std::string key(name);
    auto it = impl_->nameToIndex.find(key);
    if (it == impl_->nameToIndex.end()) return false;
    const auto& e = impl_->entries[it->second];
    outBytes.resize(static_cast<size_t>(e.dataSize));
    if (e.dataSize > 0) {
        std::memcpy(outBytes.data(),
                     impl_->dataPtr + e.dataOffset,
                     static_cast<size_t>(e.dataSize));
    }
    return true;
}

bool PakReader::Contains(std::string_view name) const noexcept {
    return impl_->nameToIndex.find(std::string(name)) != impl_->nameToIndex.end();
}

size_t PakReader::EntryCount() const noexcept {
    return impl_->entries.size();
}

const std::vector<GpakEntry>& PakReader::Entries() const noexcept {
    return impl_->entries;
}

const std::string& PakReader::errorMessage() const noexcept {
    return impl_->error;
}

std::function<bool(std::string_view, std::vector<uint8_t>&)>
PakReader::MakeSoundBankLoader() const {
    // Capture by raw pointer to Impl; the reader must outlive the
    // SoundBank that uses this loader. Documented in the header.
    const Impl* ip = impl_.get();
    return [ip](std::string_view name,
                  std::vector<uint8_t>& outBytes) -> bool {
        const std::string key(name);
        auto it = ip->nameToIndex.find(key);
        if (it == ip->nameToIndex.end()) return false;
        const auto& e = ip->entries[it->second];
        outBytes.resize(static_cast<size_t>(e.dataSize));
        if (e.dataSize > 0) {
            std::memcpy(outBytes.data(),
                         ip->dataPtr + e.dataOffset,
                         static_cast<size_t>(e.dataSize));
        }
        return true;
    };
}

// ---- PakWriter -----------------------------------------------------

PakWriter::PakWriter() = default;
PakWriter::~PakWriter() = default;

void PakWriter::AddEntry(std::string name, std::vector<uint8_t> data) {
    GpakEntry e;
    e.name       = std::move(name);
    e.dataOffset = 0;            // assigned in Write()
    e.dataSize   = data.size();
    index_.push_back(std::move(e));
    blobs_.push_back(std::move(data));
}

bool PakWriter::Write(std::string_view path) {
    err_.clear();
    if (index_.size() > kMaxEntries) {
        err_ = "too many entries; max " + std::to_string(kMaxEntries);
        return false;
    }
    {
        std::unordered_map<std::string, size_t> seen;
        for (size_t i = 0; i < index_.size(); ++i) {
            const auto& e = index_[i];
            if (e.name.empty() || e.name.size() > kMaxNameBytes) {
                err_ = "entry " + std::to_string(i) + " has invalid name";
                return false;
            }
            if (!seen.emplace(e.name, i).second) {
                err_ = "duplicate entry name '" + e.name + "'";
                return false;
            }
        }
    }

    // Layout: [header][data section][index section].
    // Compute data offsets first.
    uint64_t cursor = kHeaderSize;
    for (size_t i = 0; i < index_.size(); ++i) {
        index_[i].dataOffset = cursor;
        cursor += index_[i].dataSize;
    }
    const uint64_t indexOffset = cursor;

    GpakHeader header{};
    header.magic       = kGpakMagic;
    header.version     = kGpakVersion;
    header.entryCount  = static_cast<uint32_t>(index_.size());
    header.reserved    = 0;
    header.indexOffset = indexOffset;

    std::ofstream out((std::string(path)), std::ios::binary | std::ios::trunc);
    if (!out) {
        err_ = "could not open " + std::string(path) + " for writing";
        return false;
    }
    out.write(reinterpret_cast<const char*>(&header), kHeaderSize);
    for (size_t i = 0; i < blobs_.size(); ++i) {
        if (!blobs_[i].empty()) {
            out.write(reinterpret_cast<const char*>(blobs_[i].data()),
                       static_cast<std::streamsize>(blobs_[i].size()));
        }
    }
    for (const auto& e : index_) {
        const uint32_t nameLen = static_cast<uint32_t>(e.name.size());
        out.write(reinterpret_cast<const char*>(&nameLen),       4);
        out.write(reinterpret_cast<const char*>(&e.dataOffset),  8);
        out.write(reinterpret_cast<const char*>(&e.dataSize),    8);
        out.write(e.name.data(), static_cast<std::streamsize>(nameLen));
    }
    if (!out) {
        err_ = "write to " + std::string(path) + " failed mid-stream";
        return false;
    }
    return true;
}

const std::string& PakWriter::errorMessage() const noexcept {
    return err_;
}

} // namespace audio
