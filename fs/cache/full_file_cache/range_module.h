/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once

#include <sys/types.h>
#include <algorithm>
#include <iterator>
#include <map>
#include <utility>

namespace photon {
namespace fs {

// A disjoint interval set that supports add/remove/query operations.
// Internally stored as a sorted map: key = interval start, value = interval end.
// All intervals are half-open [left, right). Overlapping/adjacent intervals are
// automatically merged on insertion.
class RangeModule {
 public:
  RangeModule() {}

  // Add interval [left, right), merging with any overlapping or adjacent intervals.
  void addRange(off_t left, off_t right) {
    auto it = intervals.upper_bound(left);
    if (it != intervals.begin()) {
      auto start = std::prev(it);
      if (start->second >= right) {
        return;
      }
      if (start->second >= left) {
        left = start->first;
        intervals.erase(start);
      }
    }
    while (it != intervals.end() && it->first <= right) {
      right = std::max(right, it->second);
      it = intervals.erase(it);
    }
    intervals[left] = right;
  }

  // Return true if [left, right) is fully covered by existing intervals.
  bool queryRange(off_t left, off_t right) {
    auto it = intervals.upper_bound(left);
    if (it == intervals.begin()) {
      return false;
    }
    it = std::prev(it);
    return right <= it->second;
  }

  // Remove interval [left, right), splitting/trimming existing intervals as needed.
  void removeRange(off_t left, off_t right) {
    auto it = intervals.upper_bound(left);
    if (it != intervals.begin()) {
      auto start = std::prev(it);
      if (start->second >= right) {
        off_t ri = start->second;
        if (start->first == left) {
          intervals.erase(start);
        } else {
          start->second = left;
        }
        if (right != ri) {
          intervals[right] = ri;
        }
        return;
      } else if (start->second > left) {
        if (start->first == left) {
          intervals.erase(start);
        } else {
          start->second = left;
        }
      }
    }
    while (it != intervals.end() && it->first < right) {
      if (it->second <= right) {
        it = intervals.erase(it);
      } else {
        intervals[right] = it->second;
        intervals.erase(it);
        break;
      }
    }
  }

  // Remove all intervals.
  void clear() {
    intervals.clear();
  }

  // Compute the outer refill region needed to fully cover [left, right):
  // trim from the left edge if [left, ...) is already inside a filled interval,
  // trim from the right edge if (..., right) is already inside a filled interval,
  // and otherwise return [left, right) as-is — interior gaps are ignored so the
  // caller refills the whole region in one shot (matches fiemap-path semantics).
  // Returns {0, 0} when [left, right) is fully covered or empty.
  std::pair<off_t, off_t> queryRefillRange(off_t left, off_t right) {
    if (left >= right) return {0, 0};
    off_t outLeft = left;
    off_t outRight = right;
    auto it = intervals.upper_bound(left);
    if (it != intervals.begin()) {
      auto prev = std::prev(it);
      if (prev->second > left) {
        outLeft = prev->second;
      }
    }
    if (outLeft >= outRight) return {0, 0};
    auto rit = intervals.lower_bound(outRight);
    if (rit != intervals.begin()) {
      auto prev = std::prev(rit);
      if (prev->second >= outRight && prev->first > outLeft) {
        outRight = prev->first;
      }
    }
    if (outLeft >= outRight) return {0, 0};
    return {outLeft, outRight};
  }

  // Remove all intervals from offset onwards (truncate semantics).
  void removeFrom(off_t offset) {
    auto it = intervals.lower_bound(offset);
    if (it != intervals.begin()) {
      auto prev = std::prev(it);
      if (prev->second > offset) {
        prev->second = offset;
      }
    }
    intervals.erase(it, intervals.end());
  }

 private:
  std::map<off_t, off_t> intervals;  // key: start, value: end (half-open)
};

}
}
