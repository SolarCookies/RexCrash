// The log kept in memory for reports: a log sink that writes nothing to disk, and only turns its lines into text
// when a report asks for them.
//
// Two parts:
//   header  the startup lines (machine, mods, build), frozen by PinPrefix and never dropped, so they head every
//           report however long the game has run
//   ring    the most recent lines, up to a byte budget

#pragma once

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <spdlog/sinks/base_sink.h>

namespace rexcrash {

class LogBuffer final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  static constexpr size_t kHeaderBytes = 64 * 1024;

  explicit LogBuffer(size_t ring_bytes) : ring_budget_(ring_bytes) {}

  // Moves everything so far into the header. Called once the startup lines are in.
  void PinPrefix() {
    std::lock_guard lock(data_mutex_);
    for (auto& line : ring_) {
      if (header_bytes_ + line.size() + 1 > kHeaderBytes) break;
      header_bytes_ += line.size() + 1;
      header_.push_back(std::move(line));
    }
    ring_.clear();
    ring_bytes_ = 0;
  }

  // The header then the ring, oldest first, with a note where lines were dropped.
  std::string Snapshot() const {
    std::lock_guard lock(data_mutex_);
    std::string out;
    out.reserve(header_bytes_ + ring_bytes_ + 128);
    for (const auto& line : header_) out += line + '\n';
    if (dropped_) out += fmt::format("... {} earlier line(s) dropped from the in-memory log ...\n", dropped_);
    for (const auto& line : ring_) out += line + '\n';
    return out;
  }

  // Snapshot straight to a file, for the crash path: no large allocation, and a lock held by the thread that just
  // died gives up rather than hanging the crash handler.
  bool TryWriteTo(std::FILE* file) const {
    std::unique_lock lock(data_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !file) return false;
    for (const auto& line : header_) {
      std::fwrite(line.data(), 1, line.size(), file);
      std::fputc('\n', file);
    }
    for (const auto& line : ring_) {
      std::fwrite(line.data(), 1, line.size(), file);
      std::fputc('\n', file);
    }
    return true;
  }

  size_t ByteSize() const {
    std::lock_guard lock(data_mutex_);
    return header_bytes_ + ring_bytes_;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    std::string line(formatted.begin(), formatted.end());
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    std::lock_guard lock(data_mutex_);
    ring_bytes_ += line.size() + 1;
    ring_.push_back(std::move(line));
    while (ring_bytes_ > ring_budget_ && !ring_.empty()) {
      ring_bytes_ -= ring_.front().size() + 1;
      ring_.pop_front();
      ++dropped_;
    }
  }

  void flush_() override {}

 private:
  std::deque<std::string> header_;
  std::deque<std::string> ring_;
  size_t header_bytes_ = 0;
  size_t ring_bytes_ = 0;
  size_t ring_budget_;
  uint64_t dropped_ = 0;
  mutable std::mutex data_mutex_;
};

}  // namespace rexcrash
