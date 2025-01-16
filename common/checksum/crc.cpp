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

#include "crc32c.h"
#include "crc64ecma.h"
#if defined(__linux__) && defined(__aarch64__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
#include <photon/common/utility.h>
#include <photon/common/alog.h>

uint32_t (*crc32c_auto)(const uint8_t*, size_t, uint32_t) = nullptr;
uint64_t (*crc64ecma_auto)(const uint8_t *data, size_t nbytes, uint64_t crc);

__attribute__((constructor))
static void crc_init() {
#if defined(__x86_64__)
    __builtin_cpu_init();
    bool hw = __builtin_cpu_supports("sse4.2");
    crc32c_auto = hw ? crc32c_hw : crc32c_sw;
    crc64ecma_auto = hw ? crc64ecma_hw : crc64ecma_sw;
#elif defined(__aarch64__)
#ifdef __APPLE__  // apple silicon has hw for both crc
    crc32c_auto = crc32c_hw;
    crc64ecma_auto = crc64ecma_hw;
#elif defined(__linux__)  // linux on arm: runtime detection
    long hwcaps= getauxval(AT_HWCAP);
    crc32c_auto = (hwcaps & HWCAP_CRC32) ? crc32c_hw : crc32c_sw;
    crc64ecma_auto = (hwcaps & HWCAP_PMULL) ? crc64ecma_hw : crc64ecma_sw;
#else
    crc32c_auto = crc32c_sw;
    crc64ecma_auto = crc64ecma_sw;
#endif
#else // not __aarch64__, not __x86_64__
    crc32c_auto = crc32c_sw;
    crc64ecma_auto = crc64ecma_sw;
#endif
}

#if (defined(__aarch64__) && defined(__ARM_FEATURE_CRC32))
#if (defined(__clang__))
inline uint32_t crc32c(uint32_t crc, uint8_t data) {
    return __builtin_arm_crc32cb(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint16_t data) {
    return __builtin_arm_crc32ch(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint32_t data) {
    return __builtin_arm_crc32cw(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint64_t data) {
    return (uint32_t) __builtin_arm_crc32cd(crc, data);
}
#else
inline uint32_t crc32c(uint32_t crc, uint8_t data) {
    return __builtin_aarch64_crc32cb(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint16_t data) {
    return __builtin_aarch64_crc32ch(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint32_t data) {
    return __builtin_aarch64_crc32cw(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint64_t data) {
    return (uint32_t) __builtin_aarch64_crc32cx(crc, data);
}
#endif
#elif (defined(__aarch64__))
inline uint32_t crc32c(uint32_t crc, uint8_t value) {
    __asm__("crc32cb %w[c], %w[c], %w[v]":[c]"+r"(crc):[v]"r"(value));
    return crc;
}
inline uint32_t crc32c(uint32_t crc, uint16_t value) {
    __asm__("crc32ch %w[c], %w[c], %w[v]":[c]"+r"(crc):[v]"r"(value));
    return crc;
}
inline uint32_t crc32c(uint32_t crc, uint32_t value) {
    __asm__("crc32cw %w[c], %w[c], %w[v]":[c]"+r"(crc):[v]"r"(value));
    return crc;
}
inline uint32_t crc32c(uint32_t crc, uint64_t value) {
    __asm__("crc32cx %w[c], %w[c], %x[v]":[c]"+r"(crc):[v]"r"(value));
    return crc;
}
#else
inline uint32_t crc32c(uint32_t crc, uint8_t data) {
    return __builtin_ia32_crc32qi(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint16_t data) {
    return __builtin_ia32_crc32hi(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint32_t data) {
    return __builtin_ia32_crc32si(crc, data);
}
inline uint32_t crc32c(uint32_t crc, const uint64_t& data) {
    // not using the built-in intrinsic to
    // avoid an extra 64-bit to 32-bit conversion
    asm volatile ("crc32q %1, %q0" : "+r"(crc) : "rm"(data));
    return crc;
}
#endif

static const uint32_t crc32c_combine_table72[256] = {
    0x00000000,0x39d3b296,0x73a7652c,0x4a74d7ba,
    0xe74eca58,0xde9d78ce,0x94e9af74,0xad3a1de2,
    0xcb71e241,0xf2a250d7,0xb8d6876d,0x810535fb,
    0x2c3f2819,0x15ec9a8f,0x5f984d35,0x664bffa3,
    0x930fb273,0xaadc00e5,0xe0a8d75f,0xd97b65c9,
    0x7441782b,0x4d92cabd,0x07e61d07,0x3e35af91,
    0x587e5032,0x61ade2a4,0x2bd9351e,0x120a8788,
    0xbf309a6a,0x86e328fc,0xcc97ff46,0xf5444dd0,
    0x23f31217,0x1a20a081,0x5054773b,0x6987c5ad,
    0xc4bdd84f,0xfd6e6ad9,0xb71abd63,0x8ec90ff5,
    0xe882f056,0xd15142c0,0x9b25957a,0xa2f627ec,
    0x0fcc3a0e,0x361f8898,0x7c6b5f22,0x45b8edb4,
    0xb0fca064,0x892f12f2,0xc35bc548,0xfa8877de,
    0x57b26a3c,0x6e61d8aa,0x24150f10,0x1dc6bd86,
    0x7b8d4225,0x425ef0b3,0x082a2709,0x31f9959f,
    0x9cc3887d,0xa5103aeb,0xef64ed51,0xd6b75fc7,
    0x47e6242e,0x7e3596b8,0x34414102,0x0d92f394,
    0xa0a8ee76,0x997b5ce0,0xd30f8b5a,0xeadc39cc,
    0x8c97c66f,0xb54474f9,0xff30a343,0xc6e311d5,
    0x6bd90c37,0x520abea1,0x187e691b,0x21addb8d,
    0xd4e9965d,0xed3a24cb,0xa74ef371,0x9e9d41e7,
    0x33a75c05,0x0a74ee93,0x40003929,0x79d38bbf,
    0x1f98741c,0x264bc68a,0x6c3f1130,0x55eca3a6,
    0xf8d6be44,0xc1050cd2,0x8b71db68,0xb2a269fe,
    0x64153639,0x5dc684af,0x17b25315,0x2e61e183,
    0x835bfc61,0xba884ef7,0xf0fc994d,0xc92f2bdb,
    0xaf64d478,0x96b766ee,0xdcc3b154,0xe51003c2,
    0x482a1e20,0x71f9acb6,0x3b8d7b0c,0x025ec99a,
    0xf71a844a,0xcec936dc,0x84bde166,0xbd6e53f0,
    0x10544e12,0x2987fc84,0x63f32b3e,0x5a2099a8,
    0x3c6b660b,0x05b8d49d,0x4fcc0327,0x761fb1b1,
    0xdb25ac53,0xe2f61ec5,0xa882c97f,0x91517be9,
    0x8fcc485c,0xb61ffaca,0xfc6b2d70,0xc5b89fe6,
    0x68828204,0x51513092,0x1b25e728,0x22f655be,
    0x44bdaa1d,0x7d6e188b,0x371acf31,0x0ec97da7,
    0xa3f36045,0x9a20d2d3,0xd0540569,0xe987b7ff,
    0x1cc3fa2f,0x251048b9,0x6f649f03,0x56b72d95,
    0xfb8d3077,0xc25e82e1,0x882a555b,0xb1f9e7cd,
    0xd7b2186e,0xee61aaf8,0xa4157d42,0x9dc6cfd4,
    0x30fcd236,0x092f60a0,0x435bb71a,0x7a88058c,
    0xac3f5a4b,0x95ece8dd,0xdf983f67,0xe64b8df1,
    0x4b719013,0x72a22285,0x38d6f53f,0x010547a9,
    0x674eb80a,0x5e9d0a9c,0x14e9dd26,0x2d3a6fb0,
    0x80007252,0xb9d3c0c4,0xf3a7177e,0xca74a5e8,
    0x3f30e838,0x06e35aae,0x4c978d14,0x75443f82,
    0xd87e2260,0xe1ad90f6,0xabd9474c,0x920af5da,
    0xf4410a79,0xcd92b8ef,0x87e66f55,0xbe35ddc3,
    0x130fc021,0x2adc72b7,0x60a8a50d,0x597b179b,
    0xc82a6c72,0xf1f9dee4,0xbb8d095e,0x825ebbc8,
    0x2f64a62a,0x16b714bc,0x5cc3c306,0x65107190,
    0x035b8e33,0x3a883ca5,0x70fceb1f,0x492f5989,
    0xe415446b,0xddc6f6fd,0x97b22147,0xae6193d1,
    0x5b25de01,0x62f66c97,0x2882bb2d,0x115109bb,
    0xbc6b1459,0x85b8a6cf,0xcfcc7175,0xf61fc3e3,
    0x90543c40,0xa9878ed6,0xe3f3596c,0xda20ebfa,
    0x771af618,0x4ec9448e,0x04bd9334,0x3d6e21a2,
    0xebd97e65,0xd20accf3,0x987e1b49,0xa1ada9df,
    0x0c97b43d,0x354406ab,0x7f30d111,0x46e36387,
    0x20a89c24,0x197b2eb2,0x530ff908,0x6adc4b9e,
    0xc7e6567c,0xfe35e4ea,0xb4413350,0x8d9281c6,
    0x78d6cc16,0x41057e80,0x0b71a93a,0x32a21bac,
    0x9f98064e,0xa64bb4d8,0xec3f6362,0xd5ecd1f4,
    0xb3a72e57,0x8a749cc1,0xc0004b7b,0xf9d3f9ed,
    0x54e9e40f,0x6d3a5699,0x274e8123,0x1e9d33b5,
};

static const uint32_t crc32c_combine_table152[256] = {
    0x00000000,0x878a92a7,0x0af953bf,0x8d73c118,
    0x15f2a77e,0x927835d9,0x1f0bf4c1,0x98816666,
    0x2be54efc,0xac6fdc5b,0x211c1d43,0xa6968fe4,
    0x3e17e982,0xb99d7b25,0x34eeba3d,0xb364289a,
    0x57ca9df8,0xd0400f5f,0x5d33ce47,0xdab95ce0,
    0x42383a86,0xc5b2a821,0x48c16939,0xcf4bfb9e,
    0x7c2fd304,0xfba541a3,0x76d680bb,0xf15c121c,
    0x69dd747a,0xee57e6dd,0x632427c5,0xe4aeb562,
    0xaf953bf0,0x281fa957,0xa56c684f,0x22e6fae8,
    0xba679c8e,0x3ded0e29,0xb09ecf31,0x37145d96,
    0x8470750c,0x03fae7ab,0x8e8926b3,0x0903b414,
    0x9182d272,0x160840d5,0x9b7b81cd,0x1cf1136a,
    0xf85fa608,0x7fd534af,0xf2a6f5b7,0x752c6710,
    0xedad0176,0x6a2793d1,0xe75452c9,0x60dec06e,
    0xd3bae8f4,0x54307a53,0xd943bb4b,0x5ec929ec,
    0xc6484f8a,0x41c2dd2d,0xccb11c35,0x4b3b8e92,
    0x5ac60111,0xdd4c93b6,0x503f52ae,0xd7b5c009,
    0x4f34a66f,0xc8be34c8,0x45cdf5d0,0xc2476777,
    0x71234fed,0xf6a9dd4a,0x7bda1c52,0xfc508ef5,
    0x64d1e893,0xe35b7a34,0x6e28bb2c,0xe9a2298b,
    0x0d0c9ce9,0x8a860e4e,0x07f5cf56,0x807f5df1,
    0x18fe3b97,0x9f74a930,0x12076828,0x958dfa8f,
    0x26e9d215,0xa16340b2,0x2c1081aa,0xab9a130d,
    0x331b756b,0xb491e7cc,0x39e226d4,0xbe68b473,
    0xf5533ae1,0x72d9a846,0xffaa695e,0x7820fbf9,
    0xe0a19d9f,0x672b0f38,0xea58ce20,0x6dd25c87,
    0xdeb6741d,0x593ce6ba,0xd44f27a2,0x53c5b505,
    0xcb44d363,0x4cce41c4,0xc1bd80dc,0x4637127b,
    0xa299a719,0x251335be,0xa860f4a6,0x2fea6601,
    0xb76b0067,0x30e192c0,0xbd9253d8,0x3a18c17f,
    0x897ce9e5,0x0ef67b42,0x8385ba5a,0x040f28fd,
    0x9c8e4e9b,0x1b04dc3c,0x96771d24,0x11fd8f83,
    0xb58c0222,0x32069085,0xbf75519d,0x38ffc33a,
    0xa07ea55c,0x27f437fb,0xaa87f6e3,0x2d0d6444,
    0x9e694cde,0x19e3de79,0x94901f61,0x131a8dc6,
    0x8b9beba0,0x0c117907,0x8162b81f,0x06e82ab8,
    0xe2469fda,0x65cc0d7d,0xe8bfcc65,0x6f355ec2,
    0xf7b438a4,0x703eaa03,0xfd4d6b1b,0x7ac7f9bc,
    0xc9a3d126,0x4e294381,0xc35a8299,0x44d0103e,
    0xdc517658,0x5bdbe4ff,0xd6a825e7,0x5122b740,
    0x1a1939d2,0x9d93ab75,0x10e06a6d,0x976af8ca,
    0x0feb9eac,0x88610c0b,0x0512cd13,0x82985fb4,
    0x31fc772e,0xb676e589,0x3b052491,0xbc8fb636,
    0x240ed050,0xa38442f7,0x2ef783ef,0xa97d1148,
    0x4dd3a42a,0xca59368d,0x472af795,0xc0a06532,
    0x58210354,0xdfab91f3,0x52d850eb,0xd552c24c,
    0x6636ead6,0xe1bc7871,0x6ccfb969,0xeb452bce,
    0x73c44da8,0xf44edf0f,0x793d1e17,0xfeb78cb0,
    0xef4a0333,0x68c09194,0xe5b3508c,0x6239c22b,
    0xfab8a44d,0x7d3236ea,0xf041f7f2,0x77cb6555,
    0xc4af4dcf,0x4325df68,0xce561e70,0x49dc8cd7,
    0xd15deab1,0x56d77816,0xdba4b90e,0x5c2e2ba9,
    0xb8809ecb,0x3f0a0c6c,0xb279cd74,0x35f35fd3,
    0xad7239b5,0x2af8ab12,0xa78b6a0a,0x2001f8ad,
    0x9365d037,0x14ef4290,0x999c8388,0x1e16112f,
    0x86977749,0x011de5ee,0x8c6e24f6,0x0be4b651,
    0x40df38c3,0xc755aa64,0x4a266b7c,0xcdacf9db,
    0x552d9fbd,0xd2a70d1a,0x5fd4cc02,0xd85e5ea5,
    0x6b3a763f,0xecb0e498,0x61c32580,0xe649b727,
    0x7ec8d141,0xf94243e6,0x743182fe,0xf3bb1059,
    0x1715a53b,0x909f379c,0x1decf684,0x9a666423,
    0x02e70245,0x856d90e2,0x081e51fa,0x8f94c35d,
    0x3cf0ebc7,0xbb7a7960,0x3609b878,0xb1832adf,
    0x29024cb9,0xae88de1e,0x23fb1f06,0xa4718da1,
};

static const uint32_t crc32c_combine_table312[256] = {
    0x00000000,0xbac2fd7b,0x70698c07,0xcaab717c,
    0xe0d3180e,0x5a11e575,0x90ba9409,0x2a786972,
    0xc44a46ed,0x7e88bb96,0xb423caea,0x0ee13791,
    0x24995ee3,0x9e5ba398,0x54f0d2e4,0xee322f9f,
    0x8d78fb2b,0x37ba0650,0xfd11772c,0x47d38a57,
    0x6dabe325,0xd7691e5e,0x1dc26f22,0xa7009259,
    0x4932bdc6,0xf3f040bd,0x395b31c1,0x8399ccba,
    0xa9e1a5c8,0x132358b3,0xd98829cf,0x634ad4b4,
    0x1f1d80a7,0xa5df7ddc,0x6f740ca0,0xd5b6f1db,
    0xffce98a9,0x450c65d2,0x8fa714ae,0x3565e9d5,
    0xdb57c64a,0x61953b31,0xab3e4a4d,0x11fcb736,
    0x3b84de44,0x8146233f,0x4bed5243,0xf12faf38,
    0x92657b8c,0x28a786f7,0xe20cf78b,0x58ce0af0,
    0x72b66382,0xc8749ef9,0x02dfef85,0xb81d12fe,
    0x562f3d61,0xecedc01a,0x2646b166,0x9c844c1d,
    0xb6fc256f,0x0c3ed814,0xc695a968,0x7c575413,
    0x3e3b014e,0x84f9fc35,0x4e528d49,0xf4907032,
    0xdee81940,0x642ae43b,0xae819547,0x1443683c,
    0xfa7147a3,0x40b3bad8,0x8a18cba4,0x30da36df,
    0x1aa25fad,0xa060a2d6,0x6acbd3aa,0xd0092ed1,
    0xb343fa65,0x0981071e,0xc32a7662,0x79e88b19,
    0x5390e26b,0xe9521f10,0x23f96e6c,0x993b9317,
    0x7709bc88,0xcdcb41f3,0x0760308f,0xbda2cdf4,
    0x97daa486,0x2d1859fd,0xe7b32881,0x5d71d5fa,
    0x212681e9,0x9be47c92,0x514f0dee,0xeb8df095,
    0xc1f599e7,0x7b37649c,0xb19c15e0,0x0b5ee89b,
    0xe56cc704,0x5fae3a7f,0x95054b03,0x2fc7b678,
    0x05bfdf0a,0xbf7d2271,0x75d6530d,0xcf14ae76,
    0xac5e7ac2,0x169c87b9,0xdc37f6c5,0x66f50bbe,
    0x4c8d62cc,0xf64f9fb7,0x3ce4eecb,0x862613b0,
    0x68143c2f,0xd2d6c154,0x187db028,0xa2bf4d53,
    0x88c72421,0x3205d95a,0xf8aea826,0x426c555d,
    0x7c76029c,0xc6b4ffe7,0x0c1f8e9b,0xb6dd73e0,
    0x9ca51a92,0x2667e7e9,0xeccc9695,0x560e6bee,
    0xb83c4471,0x02feb90a,0xc855c876,0x7297350d,
    0x58ef5c7f,0xe22da104,0x2886d078,0x92442d03,
    0xf10ef9b7,0x4bcc04cc,0x816775b0,0x3ba588cb,
    0x11dde1b9,0xab1f1cc2,0x61b46dbe,0xdb7690c5,
    0x3544bf5a,0x8f864221,0x452d335d,0xffefce26,
    0xd597a754,0x6f555a2f,0xa5fe2b53,0x1f3cd628,
    0x636b823b,0xd9a97f40,0x13020e3c,0xa9c0f347,
    0x83b89a35,0x397a674e,0xf3d11632,0x4913eb49,
    0xa721c4d6,0x1de339ad,0xd74848d1,0x6d8ab5aa,
    0x47f2dcd8,0xfd3021a3,0x379b50df,0x8d59ada4,
    0xee137910,0x54d1846b,0x9e7af517,0x24b8086c,
    0x0ec0611e,0xb4029c65,0x7ea9ed19,0xc46b1062,
    0x2a593ffd,0x909bc286,0x5a30b3fa,0xe0f24e81,
    0xca8a27f3,0x7048da88,0xbae3abf4,0x0021568f,
    0x424d03d2,0xf88ffea9,0x32248fd5,0x88e672ae,
    0xa29e1bdc,0x185ce6a7,0xd2f797db,0x68356aa0,
    0x8607453f,0x3cc5b844,0xf66ec938,0x4cac3443,
    0x66d45d31,0xdc16a04a,0x16bdd136,0xac7f2c4d,
    0xcf35f8f9,0x75f70582,0xbf5c74fe,0x059e8985,
    0x2fe6e0f7,0x95241d8c,0x5f8f6cf0,0xe54d918b,
    0x0b7fbe14,0xb1bd436f,0x7b163213,0xc1d4cf68,
    0xebaca61a,0x516e5b61,0x9bc52a1d,0x2107d766,
    0x5d508375,0xe7927e0e,0x2d390f72,0x97fbf209,
    0xbd839b7b,0x07416600,0xcdea177c,0x7728ea07,
    0x991ac598,0x23d838e3,0xe973499f,0x53b1b4e4,
    0x79c9dd96,0xc30b20ed,0x09a05191,0xb362acea,
    0xd028785e,0x6aea8525,0xa041f459,0x1a830922,
    0x30fb6050,0x8a399d2b,0x4092ec57,0xfa50112c,
    0x14623eb3,0xaea0c3c8,0x640bb2b4,0xdec94fcf,
    0xf4b126bd,0x4e73dbc6,0x84d8aaba,0x3e1a57c1,
};

static const uint32_t crc32c_combine_table632[256] = {
    0x00000000,0x6b749fb2,0xd6e93f64,0xbd9da0d6,
    0xa83e0839,0xc34a978b,0x7ed7375d,0x15a3a8ef,
    0x55906683,0x3ee4f931,0x837959e7,0xe80dc655,
    0xfdae6eba,0x96daf108,0x2b4751de,0x4033ce6c,
    0xab20cd06,0xc05452b4,0x7dc9f262,0x16bd6dd0,
    0x031ec53f,0x686a5a8d,0xd5f7fa5b,0xbe8365e9,
    0xfeb0ab85,0x95c43437,0x285994e1,0x432d0b53,
    0x568ea3bc,0x3dfa3c0e,0x80679cd8,0xeb13036a,
    0x53adecfd,0x38d9734f,0x8544d399,0xee304c2b,
    0xfb93e4c4,0x90e77b76,0x2d7adba0,0x460e4412,
    0x063d8a7e,0x6d4915cc,0xd0d4b51a,0xbba02aa8,
    0xae038247,0xc5771df5,0x78eabd23,0x139e2291,
    0xf88d21fb,0x93f9be49,0x2e641e9f,0x4510812d,
    0x50b329c2,0x3bc7b670,0x865a16a6,0xed2e8914,
    0xad1d4778,0xc669d8ca,0x7bf4781c,0x1080e7ae,
    0x05234f41,0x6e57d0f3,0xd3ca7025,0xb8beef97,
    0xa75bd9fa,0xcc2f4648,0x71b2e69e,0x1ac6792c,
    0x0f65d1c3,0x64114e71,0xd98ceea7,0xb2f87115,
    0xf2cbbf79,0x99bf20cb,0x2422801d,0x4f561faf,
    0x5af5b740,0x318128f2,0x8c1c8824,0xe7681796,
    0x0c7b14fc,0x670f8b4e,0xda922b98,0xb1e6b42a,
    0xa4451cc5,0xcf318377,0x72ac23a1,0x19d8bc13,
    0x59eb727f,0x329fedcd,0x8f024d1b,0xe476d2a9,
    0xf1d57a46,0x9aa1e5f4,0x273c4522,0x4c48da90,
    0xf4f63507,0x9f82aab5,0x221f0a63,0x496b95d1,
    0x5cc83d3e,0x37bca28c,0x8a21025a,0xe1559de8,
    0xa1665384,0xca12cc36,0x778f6ce0,0x1cfbf352,
    0x09585bbd,0x622cc40f,0xdfb164d9,0xb4c5fb6b,
    0x5fd6f801,0x34a267b3,0x893fc765,0xe24b58d7,
    0xf7e8f038,0x9c9c6f8a,0x2101cf5c,0x4a7550ee,
    0x0a469e82,0x61320130,0xdcafa1e6,0xb7db3e54,
    0xa27896bb,0xc90c0909,0x7491a9df,0x1fe5366d,
    0x4b5bc505,0x202f5ab7,0x9db2fa61,0xf6c665d3,
    0xe365cd3c,0x8811528e,0x358cf258,0x5ef86dea,
    0x1ecba386,0x75bf3c34,0xc8229ce2,0xa3560350,
    0xb6f5abbf,0xdd81340d,0x601c94db,0x0b680b69,
    0xe07b0803,0x8b0f97b1,0x36923767,0x5de6a8d5,
    0x4845003a,0x23319f88,0x9eac3f5e,0xf5d8a0ec,
    0xb5eb6e80,0xde9ff132,0x630251e4,0x0876ce56,
    0x1dd566b9,0x76a1f90b,0xcb3c59dd,0xa048c66f,
    0x18f629f8,0x7382b64a,0xce1f169c,0xa56b892e,
    0xb0c821c1,0xdbbcbe73,0x66211ea5,0x0d558117,
    0x4d664f7b,0x2612d0c9,0x9b8f701f,0xf0fbefad,
    0xe5584742,0x8e2cd8f0,0x33b17826,0x58c5e794,
    0xb3d6e4fe,0xd8a27b4c,0x653fdb9a,0x0e4b4428,
    0x1be8ecc7,0x709c7375,0xcd01d3a3,0xa6754c11,
    0xe646827d,0x8d321dcf,0x30afbd19,0x5bdb22ab,
    0x4e788a44,0x250c15f6,0x9891b520,0xf3e52a92,
    0xec001cff,0x8774834d,0x3ae9239b,0x519dbc29,
    0x443e14c6,0x2f4a8b74,0x92d72ba2,0xf9a3b410,
    0xb9907a7c,0xd2e4e5ce,0x6f794518,0x040ddaaa,
    0x11ae7245,0x7adaedf7,0xc7474d21,0xac33d293,
    0x4720d1f9,0x2c544e4b,0x91c9ee9d,0xfabd712f,
    0xef1ed9c0,0x846a4672,0x39f7e6a4,0x52837916,
    0x12b0b77a,0x79c428c8,0xc459881e,0xaf2d17ac,
    0xba8ebf43,0xd1fa20f1,0x6c678027,0x07131f95,
    0xbfadf002,0xd4d96fb0,0x6944cf66,0x023050d4,
    0x1793f83b,0x7ce76789,0xc17ac75f,0xaa0e58ed,
    0xea3d9681,0x81490933,0x3cd4a9e5,0x57a03657,
    0x42039eb8,0x2977010a,0x94eaa1dc,0xff9e3e6e,
    0x148d3d04,0x7ff9a2b6,0xc2640260,0xa9109dd2,
    0xbcb3353d,0xd7c7aa8f,0x6a5a0a59,0x012e95eb,
    0x411d5b87,0x2a69c435,0x97f464e3,0xfc80fb51,
    0xe92353be,0x8257cc0c,0x3fca6cda,0x54bef368,
};

static const uint32_t crc32c_combine_table1272[256] = {
    0x00000000,0xdd66cbbb,0xbf21e187,0x62472a3c,
    0x7bafb5ff,0xa6c97e44,0xc48e5478,0x19e89fc3,
    0xf75f6bfe,0x2a39a045,0x487e8a79,0x951841c2,
    0x8cf0de01,0x519615ba,0x33d13f86,0xeeb7f43d,
    0xeb52a10d,0x36346ab6,0x5473408a,0x89158b31,
    0x90fd14f2,0x4d9bdf49,0x2fdcf575,0xf2ba3ece,
    0x1c0dcaf3,0xc16b0148,0xa32c2b74,0x7e4ae0cf,
    0x67a27f0c,0xbac4b4b7,0xd8839e8b,0x05e55530,
    0xd34934eb,0x0e2fff50,0x6c68d56c,0xb10e1ed7,
    0xa8e68114,0x75804aaf,0x17c76093,0xcaa1ab28,
    0x24165f15,0xf97094ae,0x9b37be92,0x46517529,
    0x5fb9eaea,0x82df2151,0xe0980b6d,0x3dfec0d6,
    0x381b95e6,0xe57d5e5d,0x873a7461,0x5a5cbfda,
    0x43b42019,0x9ed2eba2,0xfc95c19e,0x21f30a25,
    0xcf44fe18,0x122235a3,0x70651f9f,0xad03d424,
    0xb4eb4be7,0x698d805c,0x0bcaaa60,0xd6ac61db,
    0xa37e1f27,0x7e18d49c,0x1c5ffea0,0xc139351b,
    0xd8d1aad8,0x05b76163,0x67f04b5f,0xba9680e4,
    0x542174d9,0x8947bf62,0xeb00955e,0x36665ee5,
    0x2f8ec126,0xf2e80a9d,0x90af20a1,0x4dc9eb1a,
    0x482cbe2a,0x954a7591,0xf70d5fad,0x2a6b9416,
    0x33830bd5,0xeee5c06e,0x8ca2ea52,0x51c421e9,
    0xbf73d5d4,0x62151e6f,0x00523453,0xdd34ffe8,
    0xc4dc602b,0x19baab90,0x7bfd81ac,0xa69b4a17,
    0x70372bcc,0xad51e077,0xcf16ca4b,0x127001f0,
    0x0b989e33,0xd6fe5588,0xb4b97fb4,0x69dfb40f,
    0x87684032,0x5a0e8b89,0x3849a1b5,0xe52f6a0e,
    0xfcc7f5cd,0x21a13e76,0x43e6144a,0x9e80dff1,
    0x9b658ac1,0x4603417a,0x24446b46,0xf922a0fd,
    0xe0ca3f3e,0x3dacf485,0x5febdeb9,0x828d1502,
    0x6c3ae13f,0xb15c2a84,0xd31b00b8,0x0e7dcb03,
    0x179554c0,0xcaf39f7b,0xa8b4b547,0x75d27efc,
    0x431048bf,0x9e768304,0xfc31a938,0x21576283,
    0x38bffd40,0xe5d936fb,0x879e1cc7,0x5af8d77c,
    0xb44f2341,0x6929e8fa,0x0b6ec2c6,0xd608097d,
    0xcfe096be,0x12865d05,0x70c17739,0xada7bc82,
    0xa842e9b2,0x75242209,0x17630835,0xca05c38e,
    0xd3ed5c4d,0x0e8b97f6,0x6cccbdca,0xb1aa7671,
    0x5f1d824c,0x827b49f7,0xe03c63cb,0x3d5aa870,
    0x24b237b3,0xf9d4fc08,0x9b93d634,0x46f51d8f,
    0x90597c54,0x4d3fb7ef,0x2f789dd3,0xf21e5668,
    0xebf6c9ab,0x36900210,0x54d7282c,0x89b1e397,
    0x670617aa,0xba60dc11,0xd827f62d,0x05413d96,
    0x1ca9a255,0xc1cf69ee,0xa38843d2,0x7eee8869,
    0x7b0bdd59,0xa66d16e2,0xc42a3cde,0x194cf765,
    0x00a468a6,0xddc2a31d,0xbf858921,0x62e3429a,
    0x8c54b6a7,0x51327d1c,0x33755720,0xee139c9b,
    0xf7fb0358,0x2a9dc8e3,0x48dae2df,0x95bc2964,
    0xe06e5798,0x3d089c23,0x5f4fb61f,0x82297da4,
    0x9bc1e267,0x46a729dc,0x24e003e0,0xf986c85b,
    0x17313c66,0xca57f7dd,0xa810dde1,0x7576165a,
    0x6c9e8999,0xb1f84222,0xd3bf681e,0x0ed9a3a5,
    0x0b3cf695,0xd65a3d2e,0xb41d1712,0x697bdca9,
    0x7093436a,0xadf588d1,0xcfb2a2ed,0x12d46956,
    0xfc639d6b,0x210556d0,0x43427cec,0x9e24b757,
    0x87cc2894,0x5aaae32f,0x38edc913,0xe58b02a8,
    0x33276373,0xee41a8c8,0x8c0682f4,0x5160494f,
    0x4888d68c,0x95ee1d37,0xf7a9370b,0x2acffcb0,
    0xc478088d,0x191ec336,0x7b59e90a,0xa63f22b1,
    0xbfd7bd72,0x62b176c9,0x00f65cf5,0xdd90974e,
    0xd875c27e,0x051309c5,0x675423f9,0xba32e842,
    0xa3da7781,0x7ebcbc3a,0x1cfb9606,0xc19d5dbd,
    0x2f2aa980,0xf24c623b,0x900b4807,0x4d6d83bc,
    0x54851c7f,0x89e3d7c4,0xeba4fdf8,0x36c23643,
};

template<size_t begin, size_t end, ssize_t step, typename F,
        typename = typename std::enable_if<begin != end>::type>
inline __attribute__((always_inline))
void static_loop(const F& f) {
    f(begin);
    static_loop<begin + step, end, step>(f);
}

template<size_t begin, size_t end, ssize_t step, typename F, typename = void,
        typename = typename std::enable_if<begin == end>::type>
inline __attribute__((always_inline))
void static_loop(const F& f) {
    f(begin);
}

#define BODY(i) [&](size_t i) __attribute__((always_inline))

template<size_t blksz, typename T> inline __attribute__((always_inline))
void crc32c_hw_block(const uint8_t*& data, size_t& nbytes, uint32_t& crc) {
    if (nbytes & blksz) {
        static_loop<0, blksz - sizeof(T), sizeof(T)>(BODY(i) {
            crc = crc32c(crc, *(T*)(data + i));
        });
        nbytes -= blksz;
        data += blksz;
    }
}

inline __attribute__((always_inline))
void crc32c_hw_tiny(const uint8_t*& data, size_t nbytes, uint32_t& crc) {
    assert(nbytes <= 7);
    auto d = *(uint64_t*)data;
    data += nbytes;
    if (nbytes & 1) { crc = crc32c(crc, (uint8_t)d); d >>= 8; }
    if (nbytes & 2) { crc = crc32c(crc, (uint16_t)d); d >>= 16; }
    if (nbytes & 4) { crc = crc32c(crc, (uint32_t)d); }
}

inline __attribute__((always_inline))
void crc32c_hw_small(const uint8_t*& data, size_t nbytes, uint32_t& crc) {
    assert(nbytes < 256);
    if (unlikely(!nbytes || !data)) return;
    crc32c_hw_block<128, uint64_t>(data, nbytes, crc);
    crc32c_hw_block<64,  uint64_t>(data, nbytes, crc);
    crc32c_hw_block<32,  uint64_t>(data, nbytes, crc);
    crc32c_hw_block<16,  uint64_t>(data, nbytes, crc);
    crc32c_hw_block<8,   uint64_t>(data, nbytes, crc);
    crc32c_hw_tiny(data, nbytes, crc);
}

template<size_t blksz> inline __attribute__((always_inline))
void crc32c_3way_ILP(const uint8_t*& data, size_t& nbytes,
            uint32_t& crc, const uint32_t* t1, const uint32_t* t2) {
    if (nbytes < blksz * 3) return;
    auto ptr = (const uint64_t*)data;
    const size_t blksz_8 = blksz / 8;
    uint32_t crc1 = 0, crc2 = 0;
    static_loop<0, blksz_8 - 2, 1>(BODY(i) {
        if (i < blksz_8 * 3 / 4)
            __builtin_prefetch(data + blksz*3 + i*8*4, 0, 0);
        crc  = crc32c(crc,  ptr[i]);
        crc1 = crc32c(crc1, ptr[i + blksz_8]);
        crc2 = crc32c(crc2, ptr[i + blksz_8 * 2]);
    });
    crc = crc32c(crc, ptr[blksz_8 - 1]);
    crc1 = crc32c(crc1, ptr[blksz_8 * 2 - 1]);
    // crc2 = crc32c(crc2, ptr[blksz_8 * 3 - 1]);

#define pick(table, crc, shift) \
    (uint64_t(table[((crc) >> (shift)) & 0xff]) << (shift))
#define merge_in(crc, table)    \
    (pick(table, crc, 0)  ^ pick(table, crc, 8) ^ \
     pick(table, crc, 16) ^ pick(table, crc, 24))

    auto t = merge_in(crc,  t1) ^ merge_in(crc1, t2);
    crc = crc32c(crc2, ptr[blksz_8 * 3 - 1] ^ t);
    data += blksz * 3;
    nbytes -= blksz * 3;
}

// This is a portable re-imlementation of crc32_iscsi_00()
// in pure x86_64 assembly in ISA-L. It is as fast as the
// latter one for both small and big data blocks. And it is
// event faster than the ARMv8 counterpart in ISA-L, i.e.
// crc32_iscsi_crc_ext().
uint32_t crc32c_hw_portable(const uint8_t *data, size_t nbytes, uint32_t crc) {
    if (unlikely(!nbytes)) return crc;
    uint8_t l = (~((uint64_t)data) + 1) & 7;
    if (unlikely(l)) {
        if (l > nbytes) l = nbytes;
        crc32c_hw_tiny(data, l, crc);
        nbytes -= l;
    }

    while(nbytes >= 640 * 3) {
        crc32c_3way_ILP<640>(data, nbytes, crc, crc32c_combine_table1272, crc32c_combine_table632);
    }
    crc32c_3way_ILP<320>(data, nbytes, crc, crc32c_combine_table632, crc32c_combine_table312);
    crc32c_3way_ILP<160>(data, nbytes, crc, crc32c_combine_table312, crc32c_combine_table152);
    crc32c_3way_ILP<80> (data, nbytes, crc, crc32c_combine_table152, crc32c_combine_table72);
    crc32c_hw_small(data, nbytes, crc);
    return crc;
}

template<typename T, typename F1, typename F8> __attribute__((always_inline))
inline T do_crc(const uint8_t *data, size_t nbytes, T crc, F1 f1, F8 f8) {
    size_t offset = 0;
    // Process bytes one at a time until we reach an 8-byte boundary and can
    // start doing aligned 64-bit reads.
    static uintptr_t ALIGN_MASK = sizeof(uint64_t) - 1;
    size_t mask = (size_t)((uintptr_t)data & ALIGN_MASK);
    if (mask != 0) {
        size_t limit = std::min(nbytes, sizeof(uint64_t) - mask);
        while (offset < limit) {
            crc = f1(crc, data[offset]);
            offset++;
        }
    }

    // Process 8 bytes at a time until we have fewer than 8 bytes left.
    while (offset + sizeof(uint64_t) <= nbytes) {
        crc = f8(crc, *(uint64_t*)(data + offset));
        offset += sizeof(uint64_t);
    }

    // Process any bytes remaining after the last aligned 8-byte block.
    while (offset < nbytes) {
        crc = f1(crc, data[offset]);
        offset++;
    }
    return crc;
}

uint32_t crc32c_hw_simple(const uint8_t *data, size_t nbytes, uint32_t crc) {
    auto f1 = [](uint32_t crc, uint8_t b)  { return crc32c(crc, b); };
    auto f2 = [](uint32_t crc, uint64_t x) { return crc32c(crc, x); };
    return do_crc(data, nbytes, crc, f1, f2);
}

uint32_t crc32c_hw(const uint8_t *data, size_t nbytes, uint32_t crc) {
    return crc32c_hw_portable(data, nbytes, crc);
}

// rk1 ~ rk20
__attribute__((aligned(16), used))
const static uint64_t rk[20] = {
    0xdabe95afc7875f40,
    0xe05dd497ca393ae4,
    0xd7d86b2af73de740,
    0x8757d71d4fcc1000,
    0xdabe95afc7875f40,
    0x0000000000000000,
    0x9c3e466c172963d5,
    0x92d8af2baf0e1e84,
    0x947874de595052cb,
    0x9e735cb59b4724da,
    0xe4ce2cd55fea0037,
    0x2fe3fd2920ce82ec,
    0x0e31d519421a63a5,
    0x2e30203212cac325,
    0x081f6054a7842df4,
    0x6ae3efbb9dd441f3,
    0x69a35d91c3730254,
    0xb5ea1af9c013aca4,
    0x3be653a30fe1af51,
    0x60095b008a9efa44,
};

#define RK(i) &rk[i-1]

__attribute__((aligned(16), used))
const static uint64_t mask[6] = {
    0xFFFFFFFFFFFFFFFF, 0x0000000000000000,
    0xFFFFFFFF00000000, 0xFFFFFFFFFFFFFFFF,
    0x8080808080808080, 0x8080808080808080,
};

#define MASK(i) ({auto p = &mask[((i)-1)*2]; *(v128*)p;})

const static uint64_t pshufb_shf_table[4] = {
    0x8786858483828100, 0x8f8e8d8c8b8a8988,
    0x0706050403020100, 0x000e0d0c0b0a0908};

inline void* get_shf_table(size_t i) {
    return (char*)pshufb_shf_table + i;
}

#pragma GCC diagnostic ignored "-Wstrict-aliasing"

inline uint64_t mm_load_tail_tiny(const void* data, size_t n) {
    uint64_t x = 0;
    (char*&)data += n;
    if (n & 4) x = *--(const uint32_t*&)data;
    if (n & 2) x = (x<<16) | *--(const uint16_t*&)data ;
    if (n & 1) x = (x <<8) | *--(const uint8_t *&)data ;
    return x;
}

#ifdef __x86_64__
#include <immintrin.h>
#elif defined(__aarch64__)
#if !defined(__clang__) && defined(__GNUC__)
#undef __GNUC__
#define __GNUC__ 10
#endif
#define SSE2NEON_SUPPRESS_WARNINGS
#include "sse2neon.h"
#else
#error "Unsupported architecture"
#endif

struct SSE {
public:
    typedef __m128i v128;
    static v128 loadu(const void* ptr) {
        return _mm_loadu_si128((v128*)ptr);
    }
    static v128 pshufb(v128& x, const v128& y) {
        return (v128)_mm_shuffle_epi8((__m128i&)x, (const __m128i&)y);
    }
    static v128 pblendvb(v128& x, v128& y, v128& z) {
        return (v128)_mm_blendv_epi8((__m128i&)x, (__m128i&)y, (__m128i&)z);
    }
    template<uint8_t imm>
    static v128 pclmulqdq(v128& x, const uint64_t* rk) {
        return _mm_clmulepi64_si128(x, *(const v128*)rk, imm);
    }
    static v128 op(v128& x, const uint64_t* rk) {
        return pclmulqdq<0x10>(x, rk) ^ pclmulqdq<0x01>(x, rk);
    }
    static v128 load_small(const void* data, size_t n) {
        assert(n < 16);
        long x = mm_load_tail_tiny(data, n);
        return (n & 8) ? v128{*(long*)data, x} : v128{x, 0};
    }
    static v128 bsl8(v128 x) {
        return _mm_bslli_si128(x, 8);
    }
    static v128 bsr8(v128 x) {
        return _mm_bsrli_si128(x, 8);
    }
};


inline __attribute__((always_inline))
uint64_t crc64ecma_hw_portable(const uint8_t *data, size_t nbytes, uint64_t crc) {
    if (unlikely(!nbytes || !data)) return crc;
    using SIMD = SSE;
    using v128 = typename SIMD::v128;
    v128 xmm7 = {(long)~crc};
    auto& ptr = (const v128*&)data;
    if (nbytes >= 256) {
        v128 xmm[8];
        assert(nbytes >= 256);
        static_loop<0, 7, 1>(BODY(i){ xmm[i] = SIMD::loadu(ptr+i); });
        xmm[0] ^= xmm7; ptr += 8; nbytes -= 128;
        do {
            static_loop<0, 7, 1>(BODY(i) {
                xmm[i] = SIMD::op(xmm[i], RK(3)) ^ SIMD::loadu(ptr+i);
            });
            ptr += 8; nbytes -= 128;
        } while (nbytes >= 128);
        static_loop<0, 6, 1>(BODY(i) {
            auto I = (i == 6) ? 1 : (9 + i * 2);
            xmm[7] ^= SIMD::op(xmm[i], RK(I));
        });
        xmm7 = xmm[7];
    } else if (nbytes >= 16) {
        xmm7 ^= SIMD::loadu(ptr++);
        nbytes -= 16;
    } else /* 0 < nbytes < 16*/ {
        xmm7 ^= SIMD::load_small(data, nbytes);
        if (nbytes >= 8) {
            auto shf = SIMD::loadu(get_shf_table(nbytes));
            xmm7 = SIMD::pshufb(xmm7, shf);
            goto _128_done;
        } else {
            auto shf = SIMD::loadu(get_shf_table(nbytes + 8));
            xmm7 = SIMD::pshufb(xmm7, shf);
            goto _barrett;
        }
    }

    while (nbytes >= 16) {
        xmm7 = SIMD::op(xmm7, RK(1)) ^ SIMD::loadu(ptr++);
        nbytes -= 16;
    }

    if (nbytes) {
        auto p = data + nbytes - 16;
        auto remainder = SIMD::loadu((v128*)p);
        auto xmm0 = SIMD::loadu(get_shf_table(nbytes));
        auto xmm2 = xmm7;
        xmm7 = SIMD::pshufb(xmm7, xmm0);
        xmm0 ^= MASK(3);
        xmm2 = SIMD::pshufb(xmm2, xmm0);
        xmm2 = SIMD::pblendvb(xmm2, remainder, xmm0);
        xmm7 = xmm2 ^ SIMD::op(xmm7, RK(1));
    }
_128_done:
    xmm7  =  SIMD::pclmulqdq<0>(xmm7, RK(5)) ^ SIMD::bsr8(xmm7);
_barrett:
    auto t = SIMD::pclmulqdq<0>(xmm7, RK(7));
    xmm7  ^= SIMD::pclmulqdq<0x10>(t, RK(7)) ^ SIMD::bsl8(t);
    auto p = (uint64_t*)&xmm7;
    crc = ~p[1];
    return crc;
}

uint64_t crc64ecma_hw(const uint8_t *buf, size_t len, uint64_t crc) {
    return crc64ecma_hw_portable(buf, len, crc);
}

template<typename T>
struct TableCRC {
    typedef T (*Table)[256];
    Table table = (Table) malloc(sizeof(*table) * 8);
    ~TableCRC() { free(table); }
    TableCRC(T POLY) {
        for (int n = 0; n < 256; n++) {
            T crc = n;
            static_loop<0, 7, 1>(BODY(k) {
                crc = ((crc & 1) * POLY) ^ (crc >> 1);
            });
            table[0][n] = crc;
        }
        for (int n = 0; n < 256; n++) {
            T crc = table[0][n];
            static_loop<1, 7, 1>(BODY(k) {
                crc = table[0][crc & 0xff] ^ (crc >> 8);
                table[k][n] = crc;
            });
        }
    }
    __attribute__((always_inline))
    T operator()(const uint8_t *buffer, size_t nbytes, T crc) const {
        auto f1 = [&](T crc, uint8_t b) {
            return table[0][(crc ^ b) & 0xff] ^ (crc >> 8);
        };
        auto f8 = [&](T crc, uint64_t x) {
            x ^= crc; crc = 0;
            static_loop<0, 7, 1>(BODY(i) {
                crc ^= table[7-i][(x >> (i*8)) & 0xff];
            });
            return crc;
        };
        return do_crc(buffer, nbytes, crc, f1, f8);
    }
};

uint32_t crc32c_sw(const uint8_t *buffer, size_t nbytes, uint32_t crc) {
    const static TableCRC<uint32_t> calc(0x82f63b78);
    return calc(buffer, nbytes, crc);
}

uint64_t crc64ecma_sw(const uint8_t *buffer, size_t nbytes, uint64_t crc) {
    const static TableCRC<uint64_t> calc(0xc96c5795d7870f42);
    return ~calc(buffer, nbytes, ~crc);
}
