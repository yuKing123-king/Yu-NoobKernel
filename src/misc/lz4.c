#include <misc/lz4.h>
#include <misc/string.h>

#define LZ4_HASHLOG 12
#define LZ4_HASHTABLESIZE (1 << LZ4_HASHLOG)
#define LZ4_MINMATCH 4
#define LZ4_LASTLITERALS 5
#define LZ4_MFLIMIT 12

#define LZ4_HASH_FUNCTION(v)                                                   \
	(((v) * 2654435761U) >> ((32 - LZ4_HASHLOG) - LZ4_MINMATCH))

static inline u32 lz4_read32(const void *ptr) { return *(const u32 *)ptr; }

static inline size_t lz4_write_literal(u8 *op, size_t literal_len)
{
	size_t written = 0;
	if (literal_len >= 15) {
		*op++ = 15 << 4;
		literal_len -= 15;
		written = 1;
		for (; literal_len >= 255; literal_len -= 255) {
			*op++ = 255;
			written++;
		}
		*op++ = (u8)literal_len;
		written++;
	} else {
		*op++ = (u8)(literal_len << 4);
		written = 1;
	}
	return written;
}

static inline size_t lz4_write_matchlen(u8 *op, size_t match_len)
{
	size_t written = 0;
	match_len -= LZ4_MINMATCH;
	if (match_len >= 15) {
		*op++ = 15;
		match_len -= 15;
		written = 1;
		for (; match_len >= 255; match_len -= 255) {
			*op++ = 255;
			written++;
		}
		*op++ = (u8)match_len;
		written++;
	} else {
		*op++ = (u8)match_len;
		written = 1;
	}
	return written;
}

static int lz4_compress_generic(const u8 *src, int src_size, u8 *dst,
				int dst_capacity, int acceleration)
{
	const u8 *ip = src;
	const u8 *anchor = ip;
	const u8 *const iend = ip + src_size;
	const u8 *const mflimit = iend - LZ4_MFLIMIT;
	const u8 *const matchlimit = iend - LZ4_LASTLITERALS;

	u8 *op = dst;
	u8 *const oend = op + dst_capacity;

	u32 hash_table[LZ4_HASHTABLESIZE];
	memset(hash_table, 0, sizeof(hash_table));

	if (src_size < LZ4_MINMATCH) {
		goto _last_literals;
	}

	while (ip < mflimit) {
		const u8 *match;
		size_t offset;
		u32 h;

		h = LZ4_HASH_FUNCTION(lz4_read32(ip)) & (LZ4_HASHTABLESIZE - 1);
		match = src + hash_table[h];
		offset = (size_t)(ip - match);
		hash_table[h] = (u32)(ip - src);

		if (offset == 0 || offset > 65535 || match < src ||
		    lz4_read32(match) != lz4_read32(ip)) {
			ip += (acceleration > 1) ? acceleration : 1;
			continue;
		}

		{
			size_t literal_len = (size_t)(ip - anchor);
			size_t match_len = LZ4_MINMATCH;
			const u8 *match_end = ip + LZ4_MINMATCH;
			u8 *token_ptr;

			while (match_end < matchlimit &&
			       lz4_read32(match_end) ==
				   lz4_read32(match_end - offset)) {
				match_end += LZ4_MINMATCH;
			}
			while (match_end < iend &&
			       *(match_end - offset) == *match_end) {
				match_end++;
			}
			match_len = (size_t)(match_end - ip);

			token_ptr = op;
			op += lz4_write_literal(op, literal_len);

			if (op + literal_len > oend)
				return -1;
			memcpy(op, anchor, literal_len);
			op += literal_len;

			{
				size_t ml = match_len - LZ4_MINMATCH;
				*token_ptr |= (ml < 15) ? (u8)ml : 15;
			}

			if (op + 2 > oend)
				return -1;
			*op++ = (u8)(offset);
			*op++ = (u8)(offset >> 8);

			if (match_len - LZ4_MINMATCH >= 15) {
				size_t ml = match_len - LZ4_MINMATCH - 15;
				for (; ml >= 255; ml -= 255) {
					if (op >= oend)
						return -1;
					*op++ = 255;
				}
				if (op >= oend)
					return -1;
				*op++ = (u8)ml;
			}

			anchor = ip = match_end;
			continue;
		}
	}

_last_literals: {
	size_t last_run = (size_t)(iend - anchor);
	if (last_run > 0) {
		if (op + 1 + ((last_run >= 15) ? 1 : 0) +
			((last_run >= 15) ? ((last_run - 15 + 254) / 255) : 0) +
			last_run >
		    oend) {
			return -1;
		}
		{
			size_t len = last_run;
			*op++ = (u8)((len < 15) ? (len << 4) : (15 << 4));
			if (len >= 15) {
				len -= 15;
				for (; len >= 255; len -= 255)
					*op++ = 255;
				*op++ = (u8)len;
			}
		}
		memcpy(op, anchor, last_run);
		op += last_run;
	}
}

	return (int)(op - dst);
}

int lz4_compress(const void *src, int src_size, void *dst, int dst_capacity)
{
	return lz4_compress_fast(src, src_size, dst, dst_capacity, 1);
}

int lz4_compress_fast(const void *src, int src_size, void *dst,
		      int dst_capacity, int acceleration)
{
	if (src_size < 0)
		return -1;
	if (src_size == 0)
		return 0;
	if (dst_capacity < 1)
		return -1;

	return lz4_compress_generic((const u8 *)src, src_size, (u8 *)dst,
				    dst_capacity, acceleration);
}

int lz4_decompress(const void *src, int src_size, void *dst, int dst_capacity)
{
	const u8 *ip = (const u8 *)src;
	const u8 *const iend = ip + src_size;
	u8 *op = (u8 *)dst;
	u8 *const oend = op + dst_capacity;

	while (ip < iend) {
		size_t literal_len;
		size_t match_len;
		size_t offset;
		u8 token;

		token = *ip++;
		literal_len = token >> 4;
		if (literal_len == 15) {
			u8 s;
			do {
				if (ip >= iend)
					return -1;
				s = *ip++;
				literal_len += s;
			} while (s == 255);
		}

		if (ip + literal_len > iend || op + literal_len > oend)
			return -1;
		memcpy(op, ip, literal_len);
		op += literal_len;
		ip += literal_len;

		if (ip >= iend)
			break;

		if (ip + 2 > iend)
			return -1;
		offset = ip[0] | ((size_t)ip[1] << 8);
		ip += 2;
		if (offset == 0)
			return -1;

		match_len = (token & 0x0F) + LZ4_MINMATCH;
		if ((token & 0x0F) == 15) {
			u8 s;
			do {
				if (ip >= iend)
					return -1;
				s = *ip++;
				match_len += s;
			} while (s == 255);
		}

		if (op + match_len > oend)
			return -1;

		{
			u8 *dest = op;
			const u8 *ref = op - offset;
			size_t remaining = match_len;

			if (ref < (u8 *)dst)
				return -1;

			if (offset < match_len) {
				do {
					*dest++ = *ref++;
					remaining--;
				} while (remaining > offset);
				ref = dest - offset;
			}
			memcpy(dest, ref, remaining);
			op += match_len;
		}
	}

	return (int)(op - (u8 *)dst);
}
