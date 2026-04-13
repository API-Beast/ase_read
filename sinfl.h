#ifndef SINFL_H_INCLUDED
#define SINFL_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SINFL_LIT_TABLE_BITS    12
#define SINFL_DIST_TABLE_BITS   9
#define SINFL_LIT_TABLE_SIZE    (1 << SINFL_LIT_TABLE_BITS)
#define SINFL_DIST_TABLE_SIZE   (1 << SINFL_DIST_TABLE_BITS)

/* Structure aligned to cache lines for maximum L1 performance */
#if defined(__GNUC__) || defined(__clang__)
  #define SINFL_ALIGN __attribute__((aligned(64)))
#else
  #define SINFL_ALIGN __declspec(align(64))
#endif

struct SINFL_ALIGN sinfl_state {
  const unsigned char *in;
  unsigned long long bit_buf;
  int bits_left;
  /* Entry:[8b Sym2 | 8b Sym1 | 4b Type | 4b Len2 | 8b Len1] */
  unsigned *lits;
  unsigned *dists;

  unsigned dyn_lits[SINFL_LIT_TABLE_SIZE + 1500];
  unsigned dyn_dists[SINFL_DIST_TABLE_SIZE + 1500];
  unsigned fix_lits[SINFL_LIT_TABLE_SIZE + 1500];
  unsigned fix_dists[SINFL_DIST_TABLE_SIZE + 1500];
  int init_fix;
};
/* <!> input needs to be aligned to 8 byte! */
ssize_t sinflate(struct sinfl_state *state, void *out, size_t out_cap, const void *in, size_t in_size);
ssize_t zsinflate(void *out, size_t cap, const void *mem, size_t size);

#ifdef __cplusplus
}
#endif

#endif

#ifdef SINFL_IMPLEMENTATION

#define ADLER_MOD               65521
#define ADLER_NMAX              5552 /* Largest n such that s2 doesn't overflow uint32 */
#define SINFL_ADLER_INIT        (1)

/* --- Platform & Feature Selection --- */
#if !defined(SINFL_X64) && !defined(SINFL_ARM64)
  #if defined(__x86_64__) || defined(_M_X64)
    #define SINFL_X64
  #elif defined(__aarch64__) || defined(_M_ARM64)
    #define SINFL_ARM64
  #endif
#endif

#ifdef SINFL_X64
  #include <immintrin.h>
  #if defined(__BMI2__) || (defined(_MSC_VER) && defined(__AVX2__))
    #define SINFL_BMI2
  #endif
  #if defined(__AVX2__)
    #define SINFL_USE_AVX2
  #elif defined(__SSE4_2__) || defined(_M_X64)
    #define SINFL_USE_SSE42
  #endif
#elif defined(SINFL_ARM64)
  #include <arm_neon.h>
  #define SINFL_USE_NEON
#endif

#if defined(__clang__)
  #define SINFL_FORCE_INLINE inline __attribute__((always_inline))
  #define SINFL_EXPECT(x, y) __builtin_expect(!!(x), y)
  #define SINFL_BITREV16(x)  __builtin_bitreverse16((unsigned short)(x))
#elif defined(__GNUC__)
  #define SINFL_FORCE_INLINE inline __attribute__((always_inline))
  #define SINFL_EXPECT(x, y) __builtin_expect(!!(x), y)
  static inline unsigned short
  SINFL_BITREV16(unsigned short x) {
    x = ((x & 0xAAAA) >> 1) | ((x & 0x5555) << 1);
    x = ((x & 0xCCCC) >> 2) | ((x & 0x3333) << 2);
    x = ((x & 0xF0F0) >> 4) | ((x & 0x0F0F) << 4);
    return (unsigned short)((x >> 8) | (x << 8));
  }
#elif defined(_MSC_VER)
  #include <intrin.h>
  #define SINFL_FORCE_INLINE __forceinline
  #define SINFL_EXPECT(x, y) (x)
  static inline unsigned short
  SINFL_BITREV16(unsigned short x) {
    x = ((x & 0xAAAA) >> 1) | ((x & 0x5555) << 1);
    x = ((x & 0xCCCC) >> 2) | ((x & 0x3333) << 2);
    x = ((x & 0xF0F0) >> 4) | ((x & 0x0F0F) << 4);
    return (unsigned short)((x >> 8) | (x << 8));
  }
#endif

/* --- Core Primitives --- */
#define SINFL_REFILL() do { \
    unsigned long long _nv; \
    memcpy(&_nv, in, 8); \
    bit_buf |= (_nv << bits_left); \
    unsigned _ba = (unsigned)((63 - bits_left) >> 3); \
    in += _ba;\
    bits_left += (_ba << 3); \
  } while(0)

#define SINFL_REFILL_SAFE() do { \
    unsigned long long _nv = 0; \
    ptrdiff_t _left = (const unsigned char*)in_buf + in_size - in; \
    if (SINFL_EXPECT(_left >= 8, 1)) { \
      memcpy(&_nv, in, 8); \
    } else if (_left > 0) { \
      memcpy(&_nv, in, _left); \
    } else { \
      _left = 0; \
    } \
    bit_buf |= (_nv << bits_left); \
    unsigned _ba = (unsigned)((63 - bits_left) >> 3); \
    if ((ptrdiff_t)_ba > _left) _ba = (unsigned)_left; \
    in += _ba;\
    bits_left += (_ba << 3); \
  } while(0)

#ifdef SINFL_BMI2
  #define SINFL_PEEK(n)   ((unsigned)_bzhi_u64(bit_buf, (n)))
#else
  #define SINFL_PEEK(n)   ((unsigned)(bit_buf & ((1ULL << (n)) - 1)))
#endif
#define SINFL_CONSUME(n) do { \
    bit_buf >>= (n); \
    bits_left -= (n); \
    if (bits_left < 0) { bits_left = 0; bit_buf = 0; } \
  } while(0)

static SINFL_FORCE_INLINE unsigned
sinfl_rev(unsigned code, unsigned L) {
#if defined(SINFL_ARM64) && (defined(__GNUC__) || defined(__clang__))
  return __builtin_arm_rbit(code) >> (32 - L);
#else
  return (unsigned)SINFL_BITREV16((unsigned short)code) >> (16 - L);
#endif
}
static SINFL_FORCE_INLINE void
sinfl_copy_match(unsigned char *out, int dist, int len) {
  const unsigned char *src = out - dist;
  unsigned char *const end = out + len;
  if (dist < 16) {
    if (dist == 1) {
      memset(out, *src, len);
    } else {
      /* Scalar loop natively and safely handles the overlapping phase shifts */
      while (out < end) {
        *out++ = *src++;
      }
    }
    return;
  }
#if defined(SINFL_USE_AVX2)
  if (dist >= 32) {
    /* Distance >= 32: No overlap within a 32-byte chunk, raw copy */
    do {
      _mm256_storeu_si256((__m256i*)out, _mm256_loadu_si256((const __m256i*)src));
      out += 32;
      src += 32;
    } while (out < end);
  } else {
    /* Distance between 16 and 31: Overlap would occur with 32-byte chunks, use 16-byte copy */
    do {
      _mm_storeu_si128((__m128i*)out, _mm_loadu_si128((const __m128i*)src));
      out += 16;
      src += 16;
    } while (out < end);
  }
#elif defined(SINFL_USE_SSE42) || defined(SINFL_USE_NEON)
  /* Distance >= 16: No overlap within a 16-byte chunk, raw copy */
  do {
#ifdef SINFL_USE_NEON
    vst1q_u8(out, vld1q_u8(src));
#else
    _mm_storeu_si128((__m128i*)out, _mm_loadu_si128((const __m128i*)src));
#endif
    out += 16;
    src += 16;
  } while (out < end);
#else
  /* Scalar fallback: Copy byte-by-byte */
  while (out < end) {
    *out++ = *src++;
  }
#endif
}
/* --- Block Decoding --- */
enum sinfl_type {
  SINFL_TYPE_LIT = 0,
  SINFL_TYPE_MATCH = 1,
  SINFL_TYPE_EOB = 2,
  SINFL_TYPE_SUB = 3,
  SINFL_TYPE_DUAL = 4,
  SINFL_TYPE_ERR = 15,
};
static int
build_tbl(unsigned *tbl, const unsigned char *lens, int num_syms, unsigned tbl_bits) {
  unsigned cnt[17] = {0};
  unsigned start_code[17];
  /* 1. Count lengths */
  for (int i = 0; i < num_syms; i++) {
    cnt[lens[i]]++;
  }
  cnt[0] = 0;
  /* 2. Calculate start codes */
  unsigned code = 0;
  for (int i = 1; i <= 16; i++) {
    start_code[i] = code;
    code = (code + cnt[i]) << 1;
  }
  /* 3. Initialize table to ERR. Consumes 1 bit to prevent infinite loops. */
  const unsigned t_size = (1U << tbl_bits);
  memset(tbl, 0, t_size * sizeof(unsigned));
  unsigned err_ent = (SINFL_TYPE_ERR << 8) | 1;
  for (unsigned i = 0; i < t_size; i++) {
    tbl[i] = err_ent;
  }
  unsigned *sub_heap = tbl + t_size;

  /* 4. Direct single-pass table generation */
  for (int sym = 0; sym < num_syms; sym++) {
    unsigned L = lens[sym];
    if (L == 0) {
      continue;
    }
    /* Post-increment start_code[L] matches DEFLATE's lexicographical order requirement */
    unsigned c = start_code[L]++;
    unsigned rev = sinfl_rev(c, L);
    unsigned type = (sym < 256) ? SINFL_TYPE_LIT : (sym == 256 ? SINFL_TYPE_EOB : SINFL_TYPE_MATCH);
    if (L <= tbl_bits) {
      unsigned ent = (sym << 12) | (type << 8) | L;
      for (unsigned j = rev; j < t_size; j += (1U << L)) {
        tbl[j] = ent;
      }
    } else {
      unsigned prefix = rev & (t_size - 1);
      if (tbl[prefix] == err_ent) {
        tbl[prefix] = ((unsigned)(sub_heap - tbl) << 12) | (SINFL_TYPE_SUB << 8) | (15 - tbl_bits);
        unsigned sub_size = 1U << (15 - tbl_bits);
        for (unsigned k = 0; k < sub_size; k++) {
          sub_heap[k] = err_ent;
        }
        sub_heap += sub_size;
      }
      unsigned *st = tbl + (tbl[prefix] >> 12);
      unsigned ent = (sym << 12) | (type << 8) | (L - tbl_bits);
      for (unsigned j = rev >> tbl_bits; j < (1U << (tbl[prefix] & 0xFF)); j += (1U << (L - tbl_bits))) {
        st[j] = ent;
      }
    }
  }
  /* 5. Build DUAL literals if primary lit table */
  if (tbl_bits == SINFL_LIT_TABLE_BITS) {
    for (unsigned i = 0; i < t_size; i++) {
      unsigned e1 = tbl[i];
      if (((e1 >> 8) & 0xF) == SINFL_TYPE_LIT) {
        unsigned l1 = e1 & 0xFF;
        unsigned e2 = tbl[((i >> l1) & ((t_size >> l1) - 1))];
        if (((e2 >> 8) & 0xF) == SINFL_TYPE_LIT && (l1 + (e2 & 0xFF) <= SINFL_LIT_TABLE_BITS)) {
          /* OPTIMIZATION (Branchless packing):
           * sym1 is max 255. Pack sym1 at bits 12-19.
           * sym2 is max 255. Pack sym2 at bits 20-27.
           * When doing `(uint16_t)(ent >> 12)` later, little-endian machines
           * naturally map sym1 to byte 0 and sym2 to byte 1! */
          tbl[i] = ((e2 >> 12) << 20) | ((e1 >> 12) << 12) | (SINFL_TYPE_DUAL << 8) | (l1 + (e2 & 0xFF));
        }
      }
    }
  }
  return 0;
}
extern ssize_t
sinfl_decompress(struct sinfl_state *s, void *out_buf, size_t out_cap,
                 const void *in_buf, size_t in_size) {

  static const unsigned short lb[]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
  static const unsigned char le[]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
  static const unsigned short db[]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
  static const unsigned char de[]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
  static const unsigned char ord[]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

  s->init_fix = 0;
  unsigned char *out = (unsigned char*)out_buf;
  unsigned char *out_end = out + out_cap;
  unsigned char *out_limit = (out_cap >= 320) ? (out_end - 320) : out;
  const unsigned char *in = (const unsigned char*)in_buf;
  const unsigned char *in_limit = (in_size >= 8) ?
    ((const unsigned char*)in_buf + in_size - 8) :
    (const unsigned char*)in_buf;

  int last = 0;
  unsigned long long bit_buf = s->bit_buf;
  int bits_left = s->bits_left;
  while (!last) {
    SINFL_REFILL_SAFE();
    last = SINFL_PEEK(1);
    SINFL_CONSUME(1);
    int type = SINFL_PEEK(2);
    SINFL_CONSUME(2);

    if (type == 0) {
      in -= (bits_left >> 3);
      bit_buf = bits_left = 0;
      if (in + 4 > (const unsigned char*)in_buf + in_size) {
        return -1;
      }
      unsigned short len, nlen;
      memcpy(&len, in, 2);
      memcpy(&nlen, in + 2, 2);
      if (len != (unsigned short)~nlen) {
        return -1;
      }
      in += 4;
      if (out + len > out_end) {
        return -2;
      }
      if (in + len > (const unsigned char*)in_buf + in_size) {
        return -1; // Bounds check data!
      }
      memcpy(out, in, len);
      out += len;
      in += len;
    } else {
      if (type == 1) {
        if (!s->init_fix) {
          int i = 0;
          unsigned char lens[320];
          for (; i <= 143; i++) lens[i] = 8;
          for (; i <= 255; i++) lens[i] = 9;
          for (; i <= 279; i++) lens[i] = 7;
          for (; i <= 287; i++) lens[i] = 8;
          for (i = 0; i < 32; i++) lens[288+i] = 5;
          build_tbl(s->fix_lits, lens, 288, SINFL_LIT_TABLE_BITS);
          build_tbl(s->fix_dists, lens + 288, 32, SINFL_DIST_TABLE_BITS);
          s->init_fix = 1;
        }
        s->lits = s->fix_lits;
        s->dists = s->fix_dists;
      } else {
        int nlt = SINFL_PEEK(5) + 257;
        SINFL_CONSUME(5);
        int ndt = SINFL_PEEK(5) + 1;
        SINFL_CONSUME(5);
        int ncl = SINFL_PEEK(4) + 4;
        SINFL_CONSUME(4);
        unsigned char ls[320];
        unsigned char cls[19] = {0};
        for (int i = 0; i < ncl; i++) {
          SINFL_REFILL_SAFE();
          cls[ord[i]] = (unsigned char)SINFL_PEEK(3);
          SINFL_CONSUME(3);
        }
        unsigned ct[128];
        build_tbl(ct, cls, 19, 7);
        for (int i = 0; i < nlt + ndt;) {
          SINFL_REFILL_SAFE();
          unsigned e = ct[SINFL_PEEK(7)];
          SINFL_CONSUME(e & 0xFF);
          unsigned sy = e >> 12;
          if (sy < 16) {
            ls[i++] = (unsigned char)sy;
          } else {
            // Protect against code 16 (repeat prev) being first code
            if (sy == 16 && i == 0) {
              return -1;
            }
            int r = (sy == 16) ? (SINFL_PEEK(2) + 3) : (sy == 17 ? (SINFL_PEEK(3) + 3) : (SINFL_PEEK(7) + 11));
            SINFL_CONSUME((sy == 16) ? 2 : (sy == 17 ? 3 : 7));
            unsigned char v = (sy == 16) ? ls[i-1] : 0;
            if (i + r > nlt + ndt) {
              return -1;
            }
            while (r--) {
              ls[i++] = v;
            }
          }
        }
        build_tbl(s->dyn_lits, ls, nlt, SINFL_LIT_TABLE_BITS);
        build_tbl(s->dyn_dists, ls + nlt, ndt, SINFL_DIST_TABLE_BITS);

        s->lits = s->dyn_lits;
        s->dists = s->dyn_dists;
      }
      /* ==========================================================
       * FAST PATH LOOP
       * Zero bounds checks! We are guaranteed to have at least
       * 8 bytes of input and 320 bytes of output space.
       * ========================================================== */
      while (out < out_limit && in < in_limit) {
        SINFL_REFILL();
        unsigned ent = s->lits[SINFL_PEEK(SINFL_LIT_TABLE_BITS)];
        if (SINFL_EXPECT(((ent >> 8) & 0xF) == SINFL_TYPE_SUB, 0)) {
          SINFL_CONSUME(SINFL_LIT_TABLE_BITS);
          ent = s->lits[(ent >> 12) + SINFL_PEEK(ent & 0xFF)];
        }
        unsigned et = (ent >> 8) & 0xF;
        if (et == SINFL_TYPE_ERR) {
          return -1; // Corrupt stream: invalid Huffman code
        }
        if (et == SINFL_TYPE_EOB) {
          SINFL_CONSUME(ent & 0xFF);
          goto block_done;
        }
        if (et == SINFL_TYPE_LIT || et == SINFL_TYPE_DUAL) {
          /* Branchless literal packing - guaranteed safe */
          unsigned short val = (unsigned short)(ent >> 12);
          memcpy(out, &val, 2);
          out += (et == SINFL_TYPE_DUAL) ? 2 : 1;
          SINFL_CONSUME(ent & 0xFF);
          continue;
        }
        if (et == SINFL_TYPE_MATCH) {
          SINFL_CONSUME(ent & 0xFF);
          unsigned sym = (ent >> 12) - 257;
          if (SINFL_EXPECT(sym > 28, 0)) {
            return -1;
          }
          int l = lb[sym] + SINFL_PEEK(le[sym]);
          SINFL_CONSUME(le[sym]);

          unsigned dent = s->dists[SINFL_PEEK(SINFL_DIST_TABLE_BITS)];
          if (SINFL_EXPECT(((dent >> 8) & 0xF) == SINFL_TYPE_SUB, 0)) {
            SINFL_CONSUME(SINFL_DIST_TABLE_BITS);
            dent = s->dists[(dent >> 12) + SINFL_PEEK(dent & 0xFF)];
          }
          SINFL_CONSUME(dent & 0xFF);
          unsigned dsym = dent >> 12;
          if (SINFL_EXPECT(dsym > 29, 0)) {
            return -1;
          }
          int d = db[dsym] + SINFL_PEEK(de[dsym]);
          SINFL_CONSUME(de[dsym]);
          if (SINFL_EXPECT(d > (out - (unsigned char*)out_buf), 0)) {
            return -1;
          }
          sinfl_copy_match(out, d, l);
          out += l;
          continue;
        }
      }
      /* ==========================================================
       * SLOW PATH LOOP
       * Fallback for the end of the buffer where bounds checks
       * are strictly required.
       * ========================================================== */
      while (1) {
        SINFL_REFILL_SAFE();
        unsigned ent = s->lits[SINFL_PEEK(SINFL_LIT_TABLE_BITS)];
        if (SINFL_EXPECT(((ent >> 8) & 0xF) == SINFL_TYPE_SUB, 0)) {
          SINFL_CONSUME(SINFL_LIT_TABLE_BITS);
          ent = s->lits[(ent >> 12) + SINFL_PEEK(ent & 0xFF)];
        }
        unsigned et = (ent >> 8) & 0xF;
        if (et == SINFL_TYPE_ERR) {
          return -1; // Corrupt stream: invalid Huffman code
        }
        if (et == SINFL_TYPE_EOB) {
          SINFL_CONSUME(ent & 0xFF);
          goto block_done;
        }
        if (et == SINFL_TYPE_LIT || et == SINFL_TYPE_DUAL) {
          if (et == SINFL_TYPE_LIT) {
            if (SINFL_EXPECT(out < out_end, 1)) {
              *out++ = (unsigned char)(ent >> 12);
            } else {
              return -2;
            }
          } else {
            if (SINFL_EXPECT(out < out_end - 1, 1)) {
              unsigned short val = (unsigned short)(ent >> 12);
              memcpy(out, &val, 2);
              out += 2;
            } else {
              return -2;
            }
          }
          SINFL_CONSUME(ent & 0xFF);
          continue;
        }
        if (et == SINFL_TYPE_MATCH) {
          SINFL_CONSUME(ent & 0xFF);
          unsigned sym = (ent >> 12) - 257;
          if (SINFL_EXPECT(sym > 28, 0)) {
            return -1;
          }
          int l = lb[sym] + SINFL_PEEK(le[sym]);
          SINFL_CONSUME(le[sym]);

          unsigned dent = s->dists[SINFL_PEEK(SINFL_DIST_TABLE_BITS)];
          if (SINFL_EXPECT(((dent >> 8) & 0xF) == SINFL_TYPE_SUB, 0)) {
            SINFL_CONSUME(SINFL_DIST_TABLE_BITS);
            dent = s->dists[(dent >> 12) + SINFL_PEEK(dent & 0xFF)];
          }
          SINFL_CONSUME(dent & 0xFF);
          unsigned dsym = dent >> 12;
          if (SINFL_EXPECT(dsym > 29, 0)) {
            return -1;
          }
          int d = db[dsym] + SINFL_PEEK(de[dsym]);
          SINFL_CONSUME(de[dsym]);

          if (SINFL_EXPECT(out < out_limit, 1)) {
            if (SINFL_EXPECT(d > (out - (unsigned char*)out_buf), 0)) {
              return -1;
            }
            sinfl_copy_match(out, d, l);
            out += l;
          } else {
            if (SINFL_EXPECT(d > (out - (unsigned char*)out_buf), 0)) {
              return -1;
            }
            if (out + l > out_end) {
              return -2;
            }
            const unsigned char *src = out - d;
            while (l--) {
              *out++ = *src++;
            }
          }
          continue;
        }
      }
      /* Target jump for cleanly exiting either the Fast or Slow loop */
      block_done:;
    }
  }
  s->in = in;
  s->bit_buf = bit_buf;
  s->bits_left = bits_left;
  return (ssize_t)(out - (unsigned char*)out_buf);
}
extern ssize_t
sinflate(struct sinfl_state* state, void *out, size_t cap, const void *in, size_t size) {
  state->bit_buf = 0;
  state->bits_left = 0;
  state->in = (const unsigned char*)in;
  return sinfl_decompress(state, (unsigned char*)out, cap, (const unsigned char*)in, size);
}
#if defined(SINFL_USE_AVX2)
static SINFL_FORCE_INLINE int
sinfl_sum_v256(__m256i v) {
  __m128i lo = _mm256_castsi256_si128(v);
  __m128i hi = _mm256_extracti128_si256(v, 1);
  __m128i res = _mm_add_epi32(lo, hi);
  /* Fold 128-bit to 64-bit */
  res = _mm_add_epi32(res, _mm_shuffle_epi32(res, _MM_SHUFFLE(1, 0, 3, 2)));
  /* Fold 64-bit to 32-bit */
  res = _mm_add_epi32(res, _mm_shuffle_epi32(res, _MM_SHUFFLE(0, 0, 0, 1)));
  return _mm_cvtsi128_si32(res);
}
#elif defined(SINFL_USE_SSE42)
static SINFL_FORCE_INLINE int
sinfl_sum_v128(__m128i v) {
  __m128i res = _mm_add_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
  res = _mm_add_epi32(res, _mm_shuffle_epi32(res, _MM_SHUFFLE(0, 0, 0, 1)));
  return _mm_cvtsi128_si32(res);
}
#endif
static unsigned
sinfl_adler32(unsigned adler, const unsigned char *in, size_t len) {
  unsigned s1 = adler & 0xffff;
  unsigned s2 = adler >> 16;
  while (len > 0) {
    size_t tlen = (len > ADLER_NMAX) ? ADLER_NMAX : len;
    size_t remaining = tlen;
    len -= tlen;
#if defined(SINFL_USE_AVX2)
    if (remaining >= 32) {
      __m256i v_s2 = _mm256_setzero_si256();
      __m256i v_s1_vsum = _mm256_setzero_si256();
      const __m256i v_weights = _mm256_setr_epi8(
        32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
        16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1);
      while (remaining >= 32) {
        __m256i v_data = _mm256_loadu_si256((const __m256i*)in);
        v_s2 = _mm256_add_epi32(v_s2, _mm256_slli_epi32(v_s1_vsum, 5));
        v_s2 = _mm256_add_epi32(v_s2, _mm256_madd_epi16(_mm256_maddubs_epi16(v_data, v_weights), _mm256_set1_epi16(1)));
        v_s1_vsum = _mm256_add_epi32(v_s1_vsum, _mm256_sad_epu8(v_data, _mm256_setzero_si256()));
        remaining -= 32;
        in += 32;
      }
      s2 += sinfl_sum_v256(v_s2) + s1 * (unsigned)(tlen - remaining);
      s1 += sinfl_sum_v256(v_s1_vsum);
    }
#elif defined(SINFL_USE_SSE42)
    if (remaining >= 16) {
      __m128i v_s2 = _mm_setzero_si128();
      __m128i v_s1_vsum = _mm_setzero_si128();
      const __m128i v_weights = _mm_setr_epi8(16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1);
      while (remaining >= 16) {
        __m128i v_data = _mm_loadu_si128((const __m128i*)in);
        v_s2 = _mm_add_epi32(v_s2, _mm_slli_epi32(v_s1_vsum, 4));
        v_s2 = _mm_add_epi32(v_s2, _mm_madd_epi16(_mm_maddubs_epi16(v_data, v_weights), _mm_set1_epi16(1)));
        v_s1_vsum = _mm_add_epi32(v_s1_vsum, _mm_sad_epu8(v_data, _mm_setzero_si128()));
        remaining -= 16;
        in += 16;
      }
      s2 += sinfl_sum_v128(v_s2) + s1 * (unsigned)(tlen - remaining);
      s1 += sinfl_sum_v128(v_s1_vsum);
    }
#elif defined(SINFL_ARM64)
    if (remaining >= 16) {
      uint32x4_t v_s2 = vdupq_n_u32(0);
      uint32x4_t v_s1_vsum = vdupq_n_u32(0);
      const uint8x16_t v_weights = {16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1};
      while (remaining >= 16) {
        uint8x16_t v_data = vld1q_u8(in);
        /* s2 += s1 * 16 (vertical accumulation) */
        v_s2 = vaddq_u32(v_s2, vshlq_n_u32(v_s1_vsum, 4));

        /* Calculate s1 sum within block */
        uint16x8_t v_s1_16 = vpaddlq_u8(v_data);
        v_s1_vsum = vaddw_u16(v_s1_vsum, vadd_u16(vget_low_u16(v_s1_16), vget_high_u16(v_s1_16)));

        /* Weighted sum for s2: Multiply bytes by weights */
        uint16x8_t low = vmull_u8(vget_low_u8(v_data), vget_low_u8(v_weights));
        uint16x8_t high = vmull_u8(vget_high_u8(v_data), vget_high_u8(v_weights));

        /* Pairwise add 16-bit products into 32-bit s2 lanes */
        v_s2 = vaddq_u32(v_s2, vpaddlq_u16(low));
        v_s2 = vaddq_u32(v_s2, vpaddlq_u16(high));

        in += 16;
        remaining -= 16;
      }
      s2 += vaddvq_u32(v_s2) + s1 * (unsigned)(tlen - remaining);
      s1 += vaddvq_u32(v_s1_vsum);
    }
#endif
    /* Scalar tail for the chunk */
    while (remaining--) {
      s1 += *in++;
      s2 += s1;
    }
    s1 %= ADLER_MOD;
    s2 %= ADLER_MOD;
  }
  return (s2 << 16) | s1;
}
extern ssize_t
zsinflate(void *out, size_t cap, const void *mem, size_t size) {
  const unsigned char *in = (const unsigned char*)mem;
  if (size >= 6) {
    const unsigned char *eob = in + size - 4;
    struct sinfl_state s;
    s.bit_buf = 0;
    s.bits_left = 0;
    ssize_t n = sinfl_decompress(&s, out, cap, in + 2, size - 6);
    if (n < 0) {
      return -2;
    }
    unsigned a = sinfl_adler32(1u, (unsigned char*)out, (size_t)n);
    unsigned h = ((unsigned)eob[0] << 24) | ((unsigned)eob[1] << 16) | ((unsigned)eob[2] << 8) | ((unsigned)eob[3] << 0);
    return a == h ? n : -1;
  } else {
    return -1;
  }
}
#endif
