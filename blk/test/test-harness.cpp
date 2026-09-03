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

// Unit checks for the shared blk test helpers themselves (harness.h/.cpp): the
// self-describing stress block format, its validator, the failure classifier,
// and pattern().
//
// A stress test that cannot fail is worthless, and the suites' concurrent_stress
// cases only ever exercise the HAPPY path -- so this is what locks in the
// detector's sensitivity: every corruption class the driver claims to attribute
// (misrouted, torn, lost, corrupted) must actually be caught, and the SHARED
// mode's canonical-block invariant must hold, since the whole shared-region
// verification model rests on it.
//
// Pure logic: no root, no device node, no photon runtime, every platform.

#include "harness.h"

#include "../../test/gtest.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace photon {
namespace blk {
namespace test {

static constexpr size_t BLK = 64 << 10;
static constexpr uint64_t OFF = 1ull << 20;

// rewrite just the header of a formatted block, leaving the payload alone
static void patch_hdr(std::vector<char>& b, const StressBlockHdr& h) {
    memcpy(b.data(), &h, sizeof(h));
}
static StressBlockHdr hdr_of(const std::vector<char>& b) {
    StressBlockHdr h;
    memcpy(&h, b.data(), sizeof(h));
    return h;
}

TEST(StressBlock, clean_block_validates_and_reports_its_owner) {
    std::vector<char> b(BLK);
    stress_format(b.data(), BLK, OFF, 7, 3);
    EXPECT_EQ(0, stress_validate(b.data(), BLK, OFF));
    uint32_t tid = 0, seq = 0;
    ASSERT_EQ(0, stress_validate(b.data(), BLK, OFF, &tid, &seq));
    EXPECT_EQ(7u, tid);
    EXPECT_EQ(3u, seq);
}

TEST(StressBlock, misrouted_torn_and_corrupted_blocks_are_caught) {
    std::vector<char> b(BLK);
    stress_format(b.data(), BLK, OFF, 7, 3);

    // the same bytes claimed at another offset / length: a misrouted IO
    EXPECT_EQ(EILSEQ, stress_validate(b.data(), BLK, OFF + 4096));
    EXPECT_EQ(EILSEQ, stress_validate(b.data(), BLK - 4096, OFF));

    // one flipped payload byte: a torn IO
    b[BLK / 2] ^= 0x55;
    EXPECT_EQ(EILSEQ, stress_validate(b.data(), BLK, OFF));

    // a header whose length disagrees with the request
    stress_format(b.data(), BLK, OFF, 7, 3);
    auto h = hdr_of(b);
    h.len -= 4096;
    patch_hdr(b, h);
    EXPECT_EQ(EILSEQ, stress_validate(b.data(), BLK, OFF));

    // a header whose fill disagrees with its own (tid, seq)
    stress_format(b.data(), BLK, OFF, 7, 3);
    h = hdr_of(b);
    h.fill ^= 0x02;
    patch_hdr(b, h);
    EXPECT_EQ(EILSEQ, stress_validate(b.data(), BLK, OFF));
}

TEST(StressBlock, a_block_too_small_for_a_header_is_rejected) {
    std::vector<char> b(sizeof(StressBlockHdr) + 4, 0);
    EXPECT_EQ(EINVAL, stress_validate(b.data(), b.size(), OFF));
}

TEST(StressBlock, canonical_blocks_are_determined_by_the_slot_alone) {
    // the SHARED-mode invariant: two writers of one slot produce identical
    // bytes (so any interleaving is valid), and different slots differ
    std::vector<char> a(BLK), b(BLK);
    stress_format_canonical(a.data(), BLK, OFF);
    stress_format_canonical(b.data(), BLK, OFF);
    EXPECT_EQ(0, memcmp(a.data(), b.data(), BLK));
    stress_format_canonical(b.data(), BLK, OFF + 4096);
    EXPECT_NE(0, memcmp(a.data(), b.data(), BLK));
    // and the length is part of the slot's identity
    stress_format_canonical(b.data(), BLK - 4096, OFF);
    EXPECT_NE(0, memcmp(a.data(), b.data(), BLK - 4096));
}

TEST(StressBlock, poison_differs_from_every_canonical_block) {
    // a lost FIRST write to a slot is only detectable if the pre-fill cannot be
    // mistaken for content
    std::vector<char> p(BLK), c(BLK);
    stress_format_poison(p.data(), BLK);
    stress_format_canonical(c.data(), BLK, OFF);
    EXPECT_NE(0, memcmp(p.data(), c.data(), BLK));
    EXPECT_NE(0, stress_validate(p.data(), BLK, OFF));
}

TEST(StressBlock, diagnose_names_the_failure_class) {
    std::vector<char> want(BLK), got(BLK);
    stress_format_canonical(want.data(), BLK, OFF);

    stress_format_poison(got.data(), BLK);
    EXPECT_NE(std::string::npos, stress_diagnose(got.data(), BLK, OFF).find("lost"))
        << stress_diagnose(got.data(), BLK, OFF);

    stress_format_canonical(got.data(), BLK, OFF + 4096);   // another slot's block
    EXPECT_NE(std::string::npos, stress_diagnose(got.data(), BLK, OFF).find("misrouted"))
        << stress_diagnose(got.data(), BLK, OFF);

    got = want;
    auto h = hdr_of(got);
    h.fill ^= 0x02;                                         // header vs its own fill
    patch_hdr(got, h);
    EXPECT_NE(std::string::npos, stress_diagnose(got.data(), BLK, OFF).find("corrupted"))
        << stress_diagnose(got.data(), BLK, OFF);

    got = want;
    got[BLK / 3] ^= 0x77;                                   // payload only
    EXPECT_NE(std::string::npos, stress_diagnose(got.data(), BLK, OFF).find("torn"))
        << stress_diagnose(got.data(), BLK, OFF);
}

TEST(Pattern, pattern_is_deterministic_and_seed_sensitive) {
    // every suite verifies device IO against pattern(), so a collision between
    // two seeds would silently weaken those checks
    EXPECT_EQ(pattern(0x5a, 4096), pattern(0x5a, 4096));
    EXPECT_NE(pattern(0x5a, 4096), pattern(0xa5, 4096));
    EXPECT_EQ(4096u, pattern(0x5a, 4096).size());
}

}  // namespace test
}  // namespace blk
}  // namespace photon

int main(int argc, char** argv) {
    // no photon::init(): nothing here touches the runtime, which is the point --
    // the harness's verification logic must be checkable anywhere
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
