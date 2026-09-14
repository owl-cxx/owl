#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include <owl/util/base64.h>

TEST(Base64, EncodesTheRfc4648Vectors) {
    EXPECT_EQ(owl::util::base64::encode(""), "");
    EXPECT_EQ(owl::util::base64::encode("f"), "Zg==");
    EXPECT_EQ(owl::util::base64::encode("fo"), "Zm8=");
    EXPECT_EQ(owl::util::base64::encode("foo"), "Zm9v");
    EXPECT_EQ(owl::util::base64::encode("foob"), "Zm9vYg==");
    EXPECT_EQ(owl::util::base64::encode("fooba"), "Zm9vYmE=");
    EXPECT_EQ(owl::util::base64::encode("foobar"), "Zm9vYmFy");
}

TEST(Base64, EncodesWithTheStandardAlphabet) {
    EXPECT_EQ(owl::util::base64::encode(std::string("\xfb\xff\xbf", 3)), "+/+/");
    EXPECT_EQ(owl::util::base64::encode("user:pass"), "dXNlcjpwYXNz");
}

TEST(Base64, DecodesPaddedAndUnpadded) {
    EXPECT_EQ(owl::util::base64::decode("dXNlcjpwYXNz"), "user:pass");
    EXPECT_EQ(owl::util::base64::decode("YQ=="), "a");
    EXPECT_EQ(owl::util::base64::decode("YWI="), "ab");
    EXPECT_EQ(owl::util::base64::decode("YWJj"), "abc");
    EXPECT_EQ(owl::util::base64::decode("YQ"), "a");
    EXPECT_EQ(owl::util::base64::decode("YWI"), "ab");
    EXPECT_EQ(owl::util::base64::decode(""), "");
}

TEST(Base64, DecodesTheStandardAlphabet) {
    EXPECT_EQ(owl::util::base64::decode("+/+/"), std::string("\xfb\xff\xbf", 3));
}

TEST(Base64, RejectsWhatIsNotBase64) {
    EXPECT_FALSE(owl::util::base64::decode("YQ*=").has_value());
    EXPECT_FALSE(owl::util::base64::decode("Y").has_value());
    EXPECT_FALSE(owl::util::base64::decode("YQ==YQ==").has_value());
    EXPECT_FALSE(owl::util::base64::decode("YQ=").has_value());
    EXPECT_FALSE(owl::util::base64::decode("YQ===").has_value());
    EXPECT_FALSE(owl::util::base64::decode("-_-_").has_value());
    EXPECT_FALSE(owl::util::base64::decode("YW Jj").has_value());
}

TEST(Base64, RoundTripsEveryByteValue) {
    std::string all;
    for (int b = 0; b < 256; ++b) all.push_back(static_cast<char>(b));
    for (std::size_t len = 0; len <= all.size(); ++len) {
        const std::string bytes = all.substr(0, len);
        const auto decoded = owl::util::base64::decode(owl::util::base64::encode(bytes));
        ASSERT_TRUE(decoded.has_value()) << len;
        EXPECT_EQ(*decoded, bytes) << len;
    }
}
