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

// src/audio_engine/runtime/logging.cpp
//
// Implementations of the JSON Lines and Ring sinks. Independent of
// any internal-impl headers — only audio_engine/logging.h is needed.

#include "audio_engine/logging.h"

#include <cstdio>
#include <cstring>

namespace audio {

// ---------------------------------------------------------------------------
// JsonLinesLogSink
// ---------------------------------------------------------------------------

namespace {

// Append a JSON-escaped string to a buffer. Handles the small set of
// characters JSON requires escaped; non-ASCII bytes are passed through
// verbatim (UTF-8 fragments survive). Doesn't add surrounding quotes.
void AppendJsonEscaped(std::string& out, std::string_view s) {
    out.reserve(out.size() + s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    (void)std::snprintf(buf, sizeof(buf), "\\u%04x",
                                   static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
}

const char* LevelString(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
    }
    return "unknown";
}

void AppendField(std::string& out, const LogField& f) {
    out += '"';
    AppendJsonEscaped(out, f.key);
    out += "\":";
    char numbuf[64];
    switch (f.type) {
        case LogField::Type::Int64:
            (void)std::snprintf(numbuf, sizeof(numbuf), "%lld",
                           static_cast<long long>(f.value.i64));
            out += numbuf;
            break;
        case LogField::Type::UInt64:
            (void)std::snprintf(numbuf, sizeof(numbuf), "%llu",
                           static_cast<unsigned long long>(f.value.u64));
            out += numbuf;
            break;
        case LogField::Type::Float:
            // %g for compact representation; sinks that need full
            // precision can use a custom sink.
            (void)std::snprintf(numbuf, sizeof(numbuf), "%.6g", f.value.f64);
            out += numbuf;
            break;
        case LogField::Type::Bool:
            out += (f.value.b ? "true" : "false");
            break;
        case LogField::Type::StrView:
            out += '"';
            AppendJsonEscaped(out, f.value.sv);
            out += '"';
            break;
    }
}

}  // namespace

void JsonLinesLogSink::OnLogEvent(const LogEvent& event) {
    if (out_ == nullptr) return;

    // Build the line into a thread-local string then fwrite once,
    // so multi-thread emitters don't interleave inside a single line.
    // Thread-local: each thread reuses its own buffer across calls,
    // amortizing the heap allocation.
    thread_local std::string line;
    line.clear();
    line.reserve(256);

    char numbuf[32];
    line += "{\"ts\":";
    (void)std::snprintf(numbuf, sizeof(numbuf), "%llu",
                   static_cast<unsigned long long>(event.timestampMs));
    line += numbuf;

    line += ",\"level\":\"";
    line += LevelString(event.level);
    line += "\",\"category\":\"";
    AppendJsonEscaped(line, event.category);
    line += "\",\"message\":\"";
    AppendJsonEscaped(line, event.message);
    line += '"';

    for (const auto& f : event.fields) {
        line += ',';
        AppendField(line, f);
    }
    line += "}\n";

    // Single fwrite — atomic at the FD level for line sizes under
    // PIPE_BUF (4096) on POSIX. Larger lines may interleave but
    // typical structured logs stay well under this.
    (void)std::fwrite(line.data(), 1, line.size(), out_);
}

// ---------------------------------------------------------------------------
// RingLogSink
// ---------------------------------------------------------------------------
//
// Stores a deep copy of each event. The challenge is that LogField
// values of type StrView reference borrowed memory that may not
// outlive the OnLogEvent call. Solution: copy each string_view's
// bytes into the StoredEvent's stringStorage vector, then re-point
// the field's value.sv at the copied bytes. The resulting StoredEvent
// is fully self-contained.

void RingLogSink::OnLogEvent(const LogEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    StoredEvent& slot = buf_[head_];
    slot.timestampMs = event.timestampMs;
    slot.level       = event.level;
    slot.category.assign(event.category);
    slot.message.assign(event.message);
    slot.fields.clear();
    slot.fields.reserve(event.fields.size());
    slot.stringStorage.clear();
    // Reserve enough room for every potential StrView so the vector
    // doesn't reallocate mid-loop (which would invalidate the
    // string_views we re-point into it).
    slot.stringStorage.reserve(event.fields.size());

    for (const auto& f : event.fields) {
        LogField copy = f;
        if (f.type == LogField::Type::StrView) {
            slot.stringStorage.emplace_back(f.value.sv);
            const std::string& stored = slot.stringStorage.back();
            copy.value.sv = std::string_view(stored);
        }
        slot.fields.push_back(copy);
    }

    head_ = (head_ + 1) % buf_.size();
    if (size_ < buf_.size()) ++size_;
}

std::vector<RingLogSink::StoredEvent> RingLogSink::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StoredEvent> out;
    out.reserve(size_);
    if (size_ != 0) {
        const size_t cap   = buf_.size();
        const size_t start = (size_ < cap) ? 0 : head_;
        for (size_t i = 0; i < size_; ++i) {
            const StoredEvent& e = buf_[(start + i) % cap];
            // Deep copy preserves field validity for callers that hold
            // the snapshot longer than the original buf_ entry's lifetime.
            StoredEvent copy;
            copy.timestampMs   = e.timestampMs;
            copy.level         = e.level;
            copy.category      = e.category;
            copy.message       = e.message;
            copy.stringStorage = e.stringStorage;
            copy.fields.reserve(e.fields.size());
            // Re-point any StrView fields at the copied stringStorage.
            // Walk in lockstep: every StrView field consumed one entry
            // in stringStorage, in order.
            size_t storageIdx = 0;
            for (const auto& f : e.fields) {
                LogField fc = f;
                if (f.type == LogField::Type::StrView) {
                    fc.value.sv = std::string_view(copy.stringStorage[storageIdx++]);
                }
                copy.fields.push_back(fc);
            }
            out.push_back(std::move(copy));
        }
    }
    return out;
}

size_t RingLogSink::Size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

void RingLogSink::Clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    size_ = 0;
    for (auto& e : buf_) e = {};
}

}  // namespace audio
