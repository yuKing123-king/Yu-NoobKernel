#pragma once

#include <misc/stdint.h>
#include <misc/stddef.h>

#define SHA224_DIGEST_SIZE 28
#define SHA224_BLOCK_SIZE 64

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE 64

#define SHA384_DIGEST_SIZE 48
#define SHA384_BLOCK_SIZE 128

#define SHA512_DIGEST_SIZE 64
#define SHA512_BLOCK_SIZE 128

struct sha256_ctx {
	u32 state[8];
	u64 count;
	u8 buf[64];
};

struct sha512_ctx {
	u64 state[8];
	u64 count[2];
	u8 buf[128];
};

void sha224_init(struct sha256_ctx *ctx);
void sha224_update(struct sha256_ctx *ctx, const void *data, size_t len);
void sha224_final(struct sha256_ctx *ctx, u8 *digest);

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(struct sha256_ctx *ctx, u8 *digest);

void sha384_init(struct sha512_ctx *ctx);
void sha384_update(struct sha512_ctx *ctx, const void *data, size_t len);
void sha384_final(struct sha512_ctx *ctx, u8 *digest);

void sha512_init(struct sha512_ctx *ctx);
void sha512_update(struct sha512_ctx *ctx, const void *data, size_t len);
void sha512_final(struct sha512_ctx *ctx, u8 *digest);

void sha224(const void *data, size_t len, u8 *digest);
void sha256(const void *data, size_t len, u8 *digest);
void sha384(const void *data, size_t len, u8 *digest);
void sha512(const void *data, size_t len, u8 *digest);
