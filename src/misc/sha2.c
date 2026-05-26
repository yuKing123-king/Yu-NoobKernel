#include <misc/sha2.h>
#include <misc/string.h>
#include <misc/endian.h>

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define CH32(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define CH64(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ32(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define MAJ64(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

#define EP0_32(x) (ROTR32(x, 2) ^ ROTR32(x, 13) ^ ROTR32(x, 22))
#define EP1_32(x) (ROTR32(x, 6) ^ ROTR32(x, 11) ^ ROTR32(x, 25))
#define SIG0_32(x) (ROTR32(x, 7) ^ ROTR32(x, 18) ^ ((x) >> 3))
#define SIG1_32(x) (ROTR32(x, 17) ^ ROTR32(x, 19) ^ ((x) >> 10))

#define EP0_64(x) (ROTR64(x, 28) ^ ROTR64(x, 34) ^ ROTR64(x, 39))
#define EP1_64(x) (ROTR64(x, 14) ^ ROTR64(x, 18) ^ ROTR64(x, 41))
#define SIG0_64(x) (ROTR64(x, 1) ^ ROTR64(x, 8) ^ ((x) >> 7))
#define SIG1_64(x) (ROTR64(x, 19) ^ ROTR64(x, 61) ^ ((x) >> 6))

static const u32 sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static const u64 sha512_k[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

static const u32 sha224_init_state[8] = {0xc1059ed8, 0x367cd507, 0x3070dd17,
					 0xf70e5939, 0xffc00b31, 0x68581511,
					 0x64f98fa7, 0xbefa4fa4};

static const u32 sha256_init_state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
					 0xa54ff53a, 0x510e527f, 0x9b05688c,
					 0x1f83d9ab, 0x5be0cd19};

static const u64 sha384_init_state[8] = {
    0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL,
    0x152fecd8f70e5939ULL, 0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
    0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL};

static const u64 sha512_init_state[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

static void sha256_transform(struct sha256_ctx *ctx, const u8 *data)
{
	u32 w[64];
	u32 a, b, c, d, e, f, g, h;
	u32 t1, t2;
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = be32_to_cpu(((const u32 *)data)[i]);
	}
	for (i = 16; i < 64; i++) {
		w[i] = SIG1_32(w[i - 2]) + w[i - 7] + SIG0_32(w[i - 15]) +
		       w[i - 16];
	}

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];

	for (i = 0; i < 64; i++) {
		t1 = h + EP1_32(e) + CH32(e, f, g) + sha256_k[i] + w[i];
		t2 = EP0_32(a) + MAJ32(a, b, c);
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

static void sha512_transform(struct sha512_ctx *ctx, const u8 *data)
{
	u64 w[80];
	u64 a, b, c, d, e, f, g, h;
	u64 t1, t2;
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = be64_to_cpu(((const u64 *)data)[i]);
	}
	for (i = 16; i < 80; i++) {
		w[i] = SIG1_64(w[i - 2]) + w[i - 7] + SIG0_64(w[i - 15]) +
		       w[i - 16];
	}

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];

	for (i = 0; i < 80; i++) {
		t1 = h + EP1_64(e) + CH64(e, f, g) + sha512_k[i] + w[i];
		t2 = EP0_64(a) + MAJ64(a, b, c);
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

void sha224_init(struct sha256_ctx *ctx)
{
	memcpy(ctx->state, sha224_init_state, sizeof(ctx->state));
	ctx->count = 0;
}

void sha224_update(struct sha256_ctx *ctx, const void *data, size_t len)
{
	sha256_update(ctx, data, len);
}

void sha224_final(struct sha256_ctx *ctx, u8 *digest)
{
	u8 tmp[SHA256_DIGEST_SIZE];
	sha256_final(ctx, tmp);
	memcpy(digest, tmp, SHA224_DIGEST_SIZE);
}

void sha256_init(struct sha256_ctx *ctx)
{
	memcpy(ctx->state, sha256_init_state, sizeof(ctx->state));
	ctx->count = 0;
}

void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len)
{
	const u8 *p = data;
	size_t buf_used = ctx->count & 0x3f;
	size_t buf_avail;

	ctx->count += len;

	if (buf_used) {
		buf_avail = 64 - buf_used;
		if (len < buf_avail) {
			memcpy(ctx->buf + buf_used, p, len);
			return;
		}
		memcpy(ctx->buf + buf_used, p, buf_avail);
		sha256_transform(ctx, ctx->buf);
		p += buf_avail;
		len -= buf_avail;
	}

	while (len >= 64) {
		sha256_transform(ctx, p);
		p += 64;
		len -= 64;
	}

	if (len) {
		memcpy(ctx->buf, p, len);
	}
}

void sha256_final(struct sha256_ctx *ctx, u8 *digest)
{
	size_t buf_used = ctx->count & 0x3f;
	u64 bit_count = ctx->count << 3;
	int i;

	ctx->buf[buf_used++] = 0x80;

	if (buf_used > 56) {
		memset(ctx->buf + buf_used, 0, 64 - buf_used);
		sha256_transform(ctx, ctx->buf);
		buf_used = 0;
	}

	memset(ctx->buf + buf_used, 0, 56 - buf_used);

	ctx->buf[56] = (u8)(bit_count >> 56);
	ctx->buf[57] = (u8)(bit_count >> 48);
	ctx->buf[58] = (u8)(bit_count >> 40);
	ctx->buf[59] = (u8)(bit_count >> 32);
	ctx->buf[60] = (u8)(bit_count >> 24);
	ctx->buf[61] = (u8)(bit_count >> 16);
	ctx->buf[62] = (u8)(bit_count >> 8);
	ctx->buf[63] = (u8)bit_count;

	sha256_transform(ctx, ctx->buf);

	for (i = 0; i < 8; i++) {
		u32 s = ctx->state[i];
		digest[i * 4] = (u8)(s >> 24);
		digest[i * 4 + 1] = (u8)(s >> 16);
		digest[i * 4 + 2] = (u8)(s >> 8);
		digest[i * 4 + 3] = (u8)s;
	}
}

void sha384_init(struct sha512_ctx *ctx)
{
	memcpy(ctx->state, sha384_init_state, sizeof(ctx->state));
	ctx->count[0] = 0;
	ctx->count[1] = 0;
}

void sha384_update(struct sha512_ctx *ctx, const void *data, size_t len)
{
	sha512_update(ctx, data, len);
}

void sha384_final(struct sha512_ctx *ctx, u8 *digest)
{
	u8 tmp[SHA512_DIGEST_SIZE];
	sha512_final(ctx, tmp);
	memcpy(digest, tmp, SHA384_DIGEST_SIZE);
}

void sha512_init(struct sha512_ctx *ctx)
{
	memcpy(ctx->state, sha512_init_state, sizeof(ctx->state));
	ctx->count[0] = 0;
	ctx->count[1] = 0;
}

void sha512_update(struct sha512_ctx *ctx, const void *data, size_t len)
{
	const u8 *p = data;
	size_t buf_used = ctx->count[0] & 0x7f;
	size_t buf_avail;

	ctx->count[0] += len;
	if (ctx->count[0] < len) {
		ctx->count[1]++;
	}

	if (buf_used) {
		buf_avail = 128 - buf_used;
		if (len < buf_avail) {
			memcpy(ctx->buf + buf_used, p, len);
			return;
		}
		memcpy(ctx->buf + buf_used, p, buf_avail);
		sha512_transform(ctx, ctx->buf);
		p += buf_avail;
		len -= buf_avail;
	}

	while (len >= 128) {
		sha512_transform(ctx, p);
		p += 128;
		len -= 128;
	}

	if (len) {
		memcpy(ctx->buf, p, len);
	}
}

void sha512_final(struct sha512_ctx *ctx, u8 *digest)
{
	size_t buf_used = ctx->count[0] & 0x7f;
	u64 bit_count_low = ctx->count[0] << 3;
	u64 bit_count_high = (ctx->count[1] << 3) | (ctx->count[0] >> 61);
	int i;

	ctx->buf[buf_used++] = 0x80;

	if (buf_used > 112) {
		memset(ctx->buf + buf_used, 0, 128 - buf_used);
		sha512_transform(ctx, ctx->buf);
		buf_used = 0;
	}

	memset(ctx->buf + buf_used, 0, 112 - buf_used);

	ctx->buf[112] = (u8)(bit_count_high >> 56);
	ctx->buf[113] = (u8)(bit_count_high >> 48);
	ctx->buf[114] = (u8)(bit_count_high >> 40);
	ctx->buf[115] = (u8)(bit_count_high >> 32);
	ctx->buf[116] = (u8)(bit_count_high >> 24);
	ctx->buf[117] = (u8)(bit_count_high >> 16);
	ctx->buf[118] = (u8)(bit_count_high >> 8);
	ctx->buf[119] = (u8)bit_count_high;
	ctx->buf[120] = (u8)(bit_count_low >> 56);
	ctx->buf[121] = (u8)(bit_count_low >> 48);
	ctx->buf[122] = (u8)(bit_count_low >> 40);
	ctx->buf[123] = (u8)(bit_count_low >> 32);
	ctx->buf[124] = (u8)(bit_count_low >> 24);
	ctx->buf[125] = (u8)(bit_count_low >> 16);
	ctx->buf[126] = (u8)(bit_count_low >> 8);
	ctx->buf[127] = (u8)bit_count_low;

	sha512_transform(ctx, ctx->buf);

	for (i = 0; i < 8; i++) {
		u64 s = ctx->state[i];
		digest[i * 8] = (u8)(s >> 56);
		digest[i * 8 + 1] = (u8)(s >> 48);
		digest[i * 8 + 2] = (u8)(s >> 40);
		digest[i * 8 + 3] = (u8)(s >> 32);
		digest[i * 8 + 4] = (u8)(s >> 24);
		digest[i * 8 + 5] = (u8)(s >> 16);
		digest[i * 8 + 6] = (u8)(s >> 8);
		digest[i * 8 + 7] = (u8)s;
	}
}

void sha224(const void *data, size_t len, u8 *digest)
{
	struct sha256_ctx ctx;
	sha224_init(&ctx);
	sha224_update(&ctx, data, len);
	sha224_final(&ctx, digest);
}

void sha256(const void *data, size_t len, u8 *digest)
{
	struct sha256_ctx ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, data, len);
	sha256_final(&ctx, digest);
}

void sha384(const void *data, size_t len, u8 *digest)
{
	struct sha512_ctx ctx;
	sha384_init(&ctx);
	sha384_update(&ctx, data, len);
	sha384_final(&ctx, digest);
}

void sha512(const void *data, size_t len, u8 *digest)
{
	struct sha512_ctx ctx;
	sha512_init(&ctx);
	sha512_update(&ctx, data, len);
	sha512_final(&ctx, digest);
}
