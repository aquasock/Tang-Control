#include "firmware_image.h"
#include "sha256.h"

#include <assert.h>

#include <iostream>
#include <string>
#include <vector>

namespace {

std::string hex_digest(const uint8_t *data, size_t length)
{
    Sha256 sha;
    sha256_init(sha);
    sha256_update(sha, data, length);
    uint8_t digest[32];
    sha256_final(sha, digest);
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (uint8_t byte : digest) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

std::string hex_digest(const std::string &text)
{
    return hex_digest(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

void test_sha256_known_answers()
{
    // FIPS 180-4 / NIST CAVP example vectors.
    assert(hex_digest("") ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(hex_digest("abc") ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(hex_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // Streamed in uneven pieces to cover block-boundary buffering.
    Sha256 sha;
    sha256_init(sha);
    const std::string chunk(997, 'a');
    size_t remaining = 1000000;
    while (remaining != 0) {
        const size_t take = remaining < chunk.size() ? remaining : chunk.size();
        sha256_update(sha, reinterpret_cast<const uint8_t *>(chunk.data()), take);
        remaining -= take;
    }
    uint8_t digest[32];
    sha256_final(sha, digest);
    static const uint8_t million_a[32] = {
        0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92, 0x81, 0xa1, 0xc7,
        0xe2, 0x84, 0xd7, 0x3e, 0x67, 0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97,
        0x20, 0x0e, 0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0};
    for (unsigned i = 0; i < 32; ++i) {
        assert(digest[i] == million_a[i]);
    }
}

// Mirrors tests/tangctl_test.py synthetic_image().
std::vector<uint8_t> synthetic_image(uint32_t body_length)
{
    std::vector<uint8_t> image(FW_HEADER_REGION + body_length, 0);
    const char magic[] = "BFNP";
    for (unsigned i = 0; i < 4; ++i) {
        image[i] = static_cast<uint8_t>(magic[i]);
        image[8 + i] = static_cast<uint8_t>("FCFG"[i]);
        image[0x64 + i] = static_cast<uint8_t>("PCFG"[i]);
        image[0x84 + i] = static_cast<uint8_t>(body_length >> (8 * i));
    }
    const uint32_t crc = fw_crc32(image.data(), FW_BOOT_HEADER_SIZE - 4);
    for (unsigned i = 0; i < 4; ++i) {
        image[FW_BOOT_HEADER_SIZE - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
    }
    for (size_t i = FW_HEADER_REGION; i < image.size(); ++i) {
        image[i] = static_cast<uint8_t>(i * 7);
    }
    return image;
}

void test_boot_header_checks()
{
    std::vector<uint8_t> image = synthetic_image(0x2000);
    uint32_t size = 0;
    assert(fw_check_boot_header(image.data(), &size) == FwImageError::NONE);
    assert(size == image.size());
    // Same bytes and digest as the tangctl host check.
    assert(hex_digest(image.data(), image.size()) ==
           "022d6c10f4addadd2f8cdff17eae68a79074c68b8081728729e1364baf6989ee");

    std::vector<uint8_t> bad = image;
    bad[0] = 'X';
    assert(fw_check_boot_header(bad.data(), &size) == FwImageError::MAGIC);
    bad = image;
    bad[0x10] ^= 1;
    assert(fw_check_boot_header(bad.data(), &size) == FwImageError::HEADER_CRC);
    bad = synthetic_image(FW_APP_MAX_SIZE);
    assert(fw_check_boot_header(bad.data(), &size) == FwImageError::LENGTH);
    bad = synthetic_image(FW_APP_MAX_SIZE - FW_HEADER_REGION);
    assert(fw_check_boot_header(bad.data(), &size) == FwImageError::NONE);
    assert(size == FW_APP_MAX_SIZE);
}

} // namespace

int main()
{
    test_sha256_known_answers();
    test_boot_header_checks();
    std::cout << "PASS SHA-256 known answers and BL616 application image checks\n";
    return 0;
}
