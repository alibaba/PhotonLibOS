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

#include <random>
#include <algorithm>

namespace photon {
namespace fs {

/* DataTypes for random generator */
template <typename T>
class RandomValueGen {
 public:
  RandomValueGen() {}
  virtual ~RandomValueGen() {}
  virtual T next() = 0;
};

// an uniform int random generator that produces inclusive-inclusive value range
class UniformInt32RandomGen: public RandomValueGen<uint32_t> {
 public:
  UniformInt32RandomGen() = default;
  UniformInt32RandomGen(uint32_t start, uint32_t end, int seed = 1213)
      : gen_(seed), dis_(start, end) {}
  uint32_t next() override { return dis_(gen_); }
  void seed(std::mt19937::result_type val) { gen_.seed(val); }

 private:
  std::mt19937 gen_;
  std::uniform_int_distribution<uint32_t> dis_;
};

class UniformCharRandomGen : public RandomValueGen<unsigned char> {
 public:
  UniformCharRandomGen(uint32_t start, uint32_t end, int seed = 1213)
    : gen_(seed), dis_(start, end) {}
  unsigned char next() { return dis_(gen_); }

 private:
  std::mt19937 gen_;
  std::uniform_int_distribution<unsigned char> dis_;
};

}
}
