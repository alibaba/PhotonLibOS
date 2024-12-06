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
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <photon/net/utils.h>

struct Base64TestData {
		std::string  str;
		std::string  base64str;
};
static Base64TestData t[]={
		{"123456789900abcdedf", "MTIzNDU2Nzg5OTAwYWJjZGVkZg=="},
		{"WARRANTIESORCONDITIONSOFANYKINDeitherexpress", "V0FSUkFOVElFU09SQ09ORElUSU9OU09GQU5ZS0lORGVpdGhlcmV4cHJlc3M="},
		{"gogo1233sjjjjasdadjjjzxASDF", "Z29nbzEyMzNzampqamFzZGFkampqenhBU0RG"},
		{"123456789012345678901234567890123456789012345678901234", "MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0"},
		{"a", "YQ=="},
		{"ab", "YWI="},
		{"abc", "YWJj"},
		{"V0FSUkFOVElFU09SQ09ORElUSU9OU09GQU5ZS0lORGVpdGhlcmV4cHJlc3M=", "VjBGU1VrRk9WRWxGVTA5U1EwOU9SRWxVU1U5T1UwOUdRVTVaUzBsT1JHVnBkR2hsY21WNGNISmxjM009"},
		{"Z29nbzEyMzNzampqamFzZGFkampqenhBU0RG", "WjI5bmJ6RXlNek56YW1wcWFtRnpaR0ZrYW1wcWVuaEJVMFJH"}
};

TEST(Net, Base64Encode) {
	for (size_t i=0;i<sizeof(t)/sizeof(t[0]); i++ ) {
		std::string ret;
		photon::net::Base64Encode(t[i].str, ret);
	    EXPECT_EQ(ret, t[i].base64str);
	}
}
TEST(Net, Base64Decode) {
	for (size_t i=0;i<sizeof(t)/sizeof(t[0]); i++ ) {
		std::string ret;
		bool ok = photon::net::Base64Decode(t[i].base64str, ret);

		EXPECT_EQ(ret, t[i].str);
	}
}

static std::string error_cases[]={
		"====", "a", "ab", "abc", "~abc", "abc`", "abc@", "abc#", "$123", "123%","123^", "-yui", "!567","&abcd", "*abcdfdf=", ",.==", "@34=", "(==%1", "[vbv", "}", "aff|", "?123","=/12"
};

TEST(Net, Base64DecodeErrorCheck) {
	for (size_t i=0;i<sizeof(error_cases)/sizeof(error_cases[0]); i++ ) {
		std::string ret;
		bool ok = photon::net::Base64Decode(error_cases[i], ret);
		EXPECT_EQ(ok, false);
	}
}
