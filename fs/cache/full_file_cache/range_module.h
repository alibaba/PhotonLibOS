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

// A disjoint interval set: half-open [left, right) ranges keyed by start,
// merged on insert and split on partial removal.
class RangeModule {
 public:
  // Add [left, right), merging with overlapping or adjacent intervals.
  void addRange(off_t left, off_t right) {
    if (left >= right) return;
    auto it = intervals.upper_bound(left);
    if (it != intervals.begin() && std::prev(it)->second >= left) --it;
    while (it != intervals.end() && it->first <= right) {
      left = std::min(left, it->first);
      right = std::max(right, it->second);
      it = intervals.erase(it);
    }
    intervals[left] = right;
  }

  // Remove [left, right), splitting/trimming overlapping intervals as needed.
  void removeRange(off_t left, off_t right) {
    if (left >= right) return;
    auto it = intervals.upper_bound(left);
    if (it != intervals.begin() && std::prev(it)->second > left) --it;
    while (it != intervals.end() && it->first < right) {
      off_t s = it->first, e = it->second;
      it = intervals.erase(it);
      if (s < left) intervals[s] = left;            // preserve left tail
      if (e > right) intervals[right] = e;          // preserve right tail
    }
  }

  // Drop every interval starting at or after `offset` (truncate semantics).
  void removeFrom(off_t offset) {
    removeRange(offset, std::numeric_limits<off_t>::max());
  }

  void clear() { intervals.clear(); }

  // Outer refill region needed to fully cover [left, right): trim the left
  // edge if it sits inside a filled interval, trim the right edge likewise,
  // ignore interior gaps so the caller refills the whole region in one shot
  // (matches the fiemap-path semantics in cache_store).
  // Returns {0, 0} when [left, right) is fully covered or empty.
  std::pair<off_t, off_t> queryRefillRange(off_t left, off_t right) {
    if (left >= right) return {0, 0};
    if (auto it = findContaining(left); it != intervals.end())
      left = it->second;
    if (left >= right) return {0, 0};
    if (auto it = findContaining(right - 1); it != intervals.end() && it->first > left)
      right = it->first;
    // Right-trim only fires when it->first > left, so right > left holds here.
    return {left, right};
  }

 private:
  std::map<off_t, off_t> intervals;     // key: start, value: end (half-open)

  // Iterator to the interval that strictly contains `pos` (start <= pos < end),
  // or end() if none.
  std::map<off_t, off_t>::iterator findContaining(off_t pos) {
    auto it = intervals.upper_bound(pos);
    if (it == intervals.begin()) return intervals.end();
    auto prev = std::prev(it);
    return prev->second > pos ? prev : intervals.end();
  }
};

}
}
