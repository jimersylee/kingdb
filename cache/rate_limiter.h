// Copyright (c) 2014, Emmanuel Goossaert. All rights reserved.
// Use of this source code is governed by the BSD 3-Clause License,
// that can be found in the LICENSE file.

#ifndef KINGDB_RATE_LIMITER_H_
#define KINGDB_RATE_LIMITER_H_

#include "util/debug.h"
#include <inttypes.h>

#include "kingdb/kdb.h"

namespace kdb {

class RateLimiter {
 public:
  RateLimiter(uint64_t rate_limit)
    : rate_limit_(rate_limit),
      rate_arriving_(250 * 1024 * 1024),
      rate_arriving_adjusted_(0),
      epoch_last_(0),
      epoch_current_(0),
      duration_slept_(0),
      bytes_per_microsecond_(5)
  {
  }

  ~RateLimiter() {
  }

  void Tick(uint64_t bytes_arriving) {
    epoch_current_ = std::time(0);
    if (epoch_current_ != epoch_last_) {
      uint64_t rate_arriving_adjusted_last = rate_arriving_adjusted_;
      rate_arriving_adjusted_ = rate_arriving_ + bytes_per_microsecond_ * duration_slept_;
      log::trace("RateLimiter",
                 "rate_arriving_: %" PRIu64 " rate_arriving_adjusted_:%" PRIu64 " rate_arriving_adjusted_last:%" PRIu64,
                 rate_arriving_, rate_arriving_adjusted_, rate_arriving_adjusted_last);
      duration_slept_ = 0;
      rate_arriving_ = 0;
      epoch_last_ = epoch_current_;

      uint64_t rate_writing = GetWritingRate();
      double ratio = (double)rate_arriving_adjusted_ / (double)rate_writing;
      if (rate_arriving_adjusted_ > rate_writing) {
        // ratio is > 0
        if (ratio > 1.50) {
          bytes_per_microsecond_ *= 0.75;
        } else if (ratio > 1.10) {
          bytes_per_microsecond_ *= 0.95;
        } else if (ratio > 1.05) {
          bytes_per_microsecond_ *= 0.99;
        } else {
          bytes_per_microsecond_ *= 0.995;
        }
        if (bytes_per_microsecond_ <= 5) bytes_per_microsecond_ += 1;
        log::trace("WriteBuffer::WriteChunk()", "decreasing");
      } else {
        // ratio is < 0
        if (ratio < 0.5) {
          bytes_per_microsecond_ *= 1.25;
        } else if (ratio < 0.90) {
          bytes_per_microsecond_ *= 1.05;
        } else if (ratio < 0.95) {
          bytes_per_microsecond_ *= 1.01;
        } else {
          bytes_per_microsecond_ *= 1.005;
        }
        log::trace("RateLimiter", "increasing");
        if (bytes_per_microsecond_ <= 5) bytes_per_microsecond_ += 1;
      }

      log::trace("RateLimiter",
                 "limit rate: bytes_per_microsecond_: %" PRIu64 " rate_writing:%" PRIu64,
                 bytes_per_microsecond_, rate_writing);
    }

    mutex_throttling_.lock();
    rate_arriving_ += bytes_arriving;
    uint64_t sleep_microseconds = 0;
    if (bytes_per_microsecond_ > 0) {
      sleep_microseconds = bytes_arriving / bytes_per_microsecond_;
    }
    mutex_throttling_.unlock();
    if (sleep_microseconds > 50000) sleep_microseconds = 50000;

    if (sleep_microseconds) {
      log::trace("RateLimiter",
                 "bytes_per_microsecond_: %" PRIu64 ", sleep_microseconds: %" PRIu64,
                 bytes_per_microsecond_, sleep_microseconds);
      std::chrono::microseconds duration(sleep_microseconds);
      std::this_thread::sleep_for(duration);
      duration_slept_ += sleep_microseconds;
    }
  }


  void WriteStart() {
    auto epoch = std::chrono::system_clock::now().time_since_epoch();
    epoch_write_start_ = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
  }


  void WriteEnd(uint64_t num_bytes_written) {
    auto epoch = std::chrono::system_clock::now().time_since_epoch();
    epoch_write_end_ = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
    uint64_t rate_writing = num_bytes_written;
    mutex_throttling_.lock();
    if (epoch_write_start_ == epoch_write_end_) {
      rate_writing = num_bytes_written;
    } else {
      double duration = ((double)epoch_write_end_ - (double)epoch_write_start_) / 1000;
      rate_writing = (uint64_t)((double)num_bytes_written / (double)duration);
    }
    StoreWritingRate(rate_writing);
    mutex_throttling_.unlock();
  }


  void StoreWritingRate(uint64_t rate) {
    if (rates_.size() >= 10) {
      rates_.erase(rates_.begin());
    }
    rates_.push_back(rate);
  }

  uint64_t GetWritingRate() {
    if (rates_.size() == 0) return 1 * 1024 * 1024;
    uint64_t sum = 0; 
    for (int i = 0; i < rates_.size(); i++) {
      sum += rates_[i]; 
    }
    uint64_t rate_limit_current = sum / rates_.size();
    if (rate_limit_ > 0 && rate_limit_ < rate_limit_current) {
      return rate_limit_;
    } else {
      return rate_limit_current;
    }
  }

  uint64_t epoch_write_start_;
  uint64_t epoch_write_end_;
  uint64_t rate_limit_;
  uint64_t rate_arriving_;
  uint64_t rate_arriving_adjusted_;
  uint64_t rate_writing_;
  uint64_t epoch_last_;
  uint64_t epoch_current_;
  uint64_t duration_slept_;
  uint64_t bytes_per_microsecond_ = 5;
  std::mutex mutex_throttling_;
  std::vector<uint64_t> rates_;
};

} // namespace kdb

#endif // KINGDB_RATE_LIMITER_H_
