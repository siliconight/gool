#ifndef AUDIO_ENGINE_GPAK_H
#define AUDIO_ENGINE_GPAK_H

// Gpak: simple, deterministic, uncompressed pack format for shipping
// sound assets in a single file.
//
// Why this exists: SoundBank's `fileLoader` callback is a hook, not
// a format. Without a shipped pack, every adopter rolls their own
// (different filenames, different headers, different APIs), and
// none of them are compatible. Gpak is the smallest format that
// answers "where does this engine pack its WAVs?" with one URL.
//
// Format (binary, little-endian, no padding):
//
//   Header (24 bytes at offset 0):
//     0..3   : magic "GPAK"
//     4..7   : version (uint32_t, currently 1)
//     8..11  : entry count (uint32_t)
//     12..15 : reserved (uint32_t, must be 0)
//     16..23 : index offset from start of file (uint64_t)
//
//   Data section (at offset 24):
//     Concatenated raw file bytes, one entry after another. No
//     padding between entries; the index records exact byte offsets.
//
//   Index section (at `header.indexOffset`):
//     For each of `header.entryCount` entries:
//       uint32_t  nameLength      (bytes, max 4096)
//       uint64_t  dataOffset      (from start of file)
//       uint64_t  dataSize        (bytes)
//       <bytes>   name            (nameLength bytes, no terminator)
//
// Entries are in insertion order. Lookups are O(N) on a linear
// scan during PakReader::Open (which builds an in-memory hash map),
// then O(1) afterward. The format is deliberately uncompressed:
// audio assets are already compressed (Ogg/FLAC) and the engine
// streams large files directly; layering general-purpose
// compression underneath would duplicate work and slow loads.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace audio {

constexpr uint32_t kGpakMagic   = 0x4B415047u;   // 'GPAK' little-endian
constexpr uint32_t kGpakVersion = 1u;

struct GpakHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t entryCount;
    uint32_t reserved;
    uint64_t indexOffset;
};
static_assert(sizeof(GpakHeader) == 24, "GpakHeader must be 24 bytes");

struct GpakEntry {
    std::string name;        // path string as stored
    uint64_t    dataOffset;  // absolute offset in the .gpak file
    uint64_t    dataSize;    // exact byte count
};

// Reader: opens a .gpak by either reading the whole file into memory
// (call Open(path)) or by referencing an external buffer (call
// OpenInMemory(span)). After Open*, Read(name, outBytes) populates
// outBytes with the file's raw bytes; nullptr/empty means "not found".
//
// Threadsafe-for-read after Open completes; Read() does no
// allocations on the hot path beyond `outBytes.resize`.
class PakReader {
public:
    PakReader();
    ~PakReader();

    PakReader(const PakReader&)            = delete;
    PakReader& operator=(const PakReader&) = delete;
    PakReader(PakReader&&) noexcept;
    PakReader& operator=(PakReader&&) noexcept;

    // Load a .gpak from disk. Reads the whole file into memory.
    // Returns true on success; on failure, errorMessage() describes why.
    bool Open(std::string_view path);

    // Reference an externally-owned byte buffer. Caller must keep
    // the buffer alive for the reader's lifetime.
    bool OpenInMemory(const uint8_t* data, size_t size);

    // Look up an entry by exact name. Populates `outBytes` (resized
    // to fit) on success and returns true. False if the name isn't
    // in the index.
    bool Read(std::string_view name, std::vector<uint8_t>& outBytes) const;

    // True if `name` is in the pack.
    bool Contains(std::string_view name) const noexcept;

    // Number of entries in the pack.
    size_t EntryCount() const noexcept;

    // Iterate the entries in insertion order. Useful for tooling
    // (e.g. listing pack contents) and doesn't allocate.
    const std::vector<GpakEntry>& Entries() const noexcept;

    // Last error message from a failed Open*.
    const std::string& errorMessage() const noexcept;

    // Adapter that returns a SoundBank-compatible file loader
    // backed by this pack. The loader treats `path` from the JSON
    // as the entry name; missing entries make the bank's load fail
    // with the standard "failed to load file" error.
    //
    // Usage:
    //   audio::PakReader pak;
    //   pak.Open("game.gpak");
    //   audio::SoundBankLoadOptions opts;
    //   opts.fileLoader = pak.MakeSoundBankLoader();
    //   bank.LoadFromJsonFile(rt, "sounds.json", opts);
    std::function<bool(std::string_view, std::vector<uint8_t>&)>
    MakeSoundBankLoader() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Writer: builds a .gpak file from a list of (name, bytes) entries.
// Use the standalone tool (`gpak_create`) for typical packing, or
// call this directly to integrate into a host's build pipeline.
class PakWriter {
public:
    PakWriter();
    ~PakWriter();

    // Add an entry with the given name and contents. Names should
    // not contain nulls; embedded slashes are allowed and treated as
    // opaque path strings (the reader does no path-normalization).
    // Names must be unique within a pack; duplicates make Write
    // fail.
    void AddEntry(std::string name, std::vector<uint8_t> data);

    // Serialize the pack to disk. Returns true on success;
    // errorMessage() describes failure.
    bool Write(std::string_view path);

    const std::string& errorMessage() const noexcept;

private:
    std::vector<GpakEntry>           index_;
    std::vector<std::vector<uint8_t>> blobs_;
    std::string                       err_;
};

} // namespace audio

#endif // AUDIO_ENGINE_GPAK_H
