#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <openssl/evp.h>
#include <openssl/hmac.h>

static inline void hmac_sha256(std::span<const uint8_t> key, std::span<const uint8_t> msg, std::span<uint8_t, 32> out) {
    unsigned int len = 32;
    HMAC(EVP_sha256(), key.data(), (int)key.size(), msg.data(), (int)msg.size(), out.data(), &len);
}

static inline int hmac_verify(std::span<const uint8_t> key, std::span<const uint8_t> msg, std::span<const uint8_t> tag) {
    if (tag.size() != 16 && tag.size() != 32) return -1;
    uint8_t computed[32];
    hmac_sha256(key, msg, computed);
    uint8_t diff = 0;
    for (size_t i = 0; i < tag.size(); ++i)
        diff |= computed[i] ^ tag[i];
    return diff == 0 ? 0 : -1;
}

static inline void derive_key(const char *secret, uint8_t key[32]) {
    unsigned int len = 32;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, secret, std::strlen(secret));
    EVP_DigestFinal_ex(ctx, key, &len);
    EVP_MD_CTX_free(ctx);
}
