// taken from zlib-ng / Intel's zlib patch, modified to remove zlib dependencies
/*
 * Compute the CRC32 using a parallelized folding approach with the PCLMULQDQ 
 * instruction.
 *
 * A white paper describing this algorithm can be found at:
 * http://www.intel.com/content/dam/www/public/us/en/documents/white-papers/fast-crc-computation-generic-polynomials-pclmulqdq-paper.pdf
 *
 * Copyright (C) 2013 Intel Corporation. All rights reserved.
 * Authors:
 *     Wajdi Feghali   <wajdi.k.feghali@intel.com>
 *     Jim Guilford    <james.guilford@intel.com>
 *     Vinodh Gopal    <vinodh.gopal@intel.com>
 *     Erdinc Ozturk   <erdinc.ozturk@intel.com>
 *     Jim Kukunas     <james.t.kukunas@linux.intel.com>
 *
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "crc_common.h"

#if (defined(__PCLMUL__) && defined(__SSSE3__) && defined(__SSE4_1__)) || (defined(_MSC_VER) && _MSC_VER >= 1600 && defined(PLATFORM_X86))
#include <inttypes.h>
#include <immintrin.h>
#include <wmmintrin.h>


#if defined(__AVX512VL__) && defined(YENC_BUILD_NATIVE) && YENC_BUILD_NATIVE!=0
# define ENABLE_AVX512 1
#endif


// interestingly, MSVC seems to generate better code if using VXORPS over VPXOR
// original Intel code uses XORPS for many XOR operations, but PXOR is pretty much always better (more port freedom on Intel CPUs). The only advantage of XORPS is that it's 1 byte shorter, an advantage which disappears under AVX as both instructions have the same length
#if defined(__AVX__) && defined(YENC_BUILD_NATIVE) && YENC_BUILD_NATIVE!=0
# define fold_xor _mm_xor_si128
#else
static __m128i fold_xor(__m128i a, __m128i b) {
    return _mm_castps_si128(_mm_xor_ps(_mm_castsi128_ps(a), _mm_castsi128_ps(b)));
}
#endif

#ifdef ENABLE_AVX512
static __m128i do_one_fold_merge(__m128i src, __m128i data) {
    const __m128i xmm_fold4 = _mm_set_epi32(
            0x00000001, 0x54442bd4,
            0x00000001, 0xc6e41596);
    return _mm_ternarylogic_epi32(
      _mm_clmulepi64_si128(src, xmm_fold4, 0x01),
      _mm_clmulepi64_si128(src, xmm_fold4, 0x10),
      data,
      0x96
    );
}
#else
static __m128i do_one_fold(__m128i src) {
    const __m128i xmm_fold4 = _mm_set_epi32(
            0x00000001, 0x54442bd4,
            0x00000001, 0xc6e41596);
    return fold_xor(
      _mm_clmulepi64_si128(src, xmm_fold4, 0x01),
      _mm_clmulepi64_si128(src, xmm_fold4, 0x10)
    );
}
#endif

ALIGN_TO(32, static const unsigned  pshufb_shf_table[60]) = {
    0x84838281, 0x88878685, 0x8c8b8a89, 0x008f8e8d, /* shl 15 (16 - 1)/shr1 */
    0x85848382, 0x89888786, 0x8d8c8b8a, 0x01008f8e, /* shl 14 (16 - 3)/shr2 */
    0x86858483, 0x8a898887, 0x8e8d8c8b, 0x0201008f, /* shl 13 (16 - 4)/shr3 */
    0x87868584, 0x8b8a8988, 0x8f8e8d8c, 0x03020100, /* shl 12 (16 - 4)/shr4 */
    0x88878685, 0x8c8b8a89, 0x008f8e8d, 0x04030201, /* shl 11 (16 - 5)/shr5 */
    0x89888786, 0x8d8c8b8a, 0x01008f8e, 0x05040302, /* shl 10 (16 - 6)/shr6 */
    0x8a898887, 0x8e8d8c8b, 0x0201008f, 0x06050403, /* shl  9 (16 - 7)/shr7 */
    0x8b8a8988, 0x8f8e8d8c, 0x03020100, 0x07060504, /* shl  8 (16 - 8)/shr8 */
    0x8c8b8a89, 0x008f8e8d, 0x04030201, 0x08070605, /* shl  7 (16 - 9)/shr9 */
    0x8d8c8b8a, 0x01008f8e, 0x05040302, 0x09080706, /* shl  6 (16 -10)/shr10*/
    0x8e8d8c8b, 0x0201008f, 0x06050403, 0x0a090807, /* shl  5 (16 -11)/shr11*/
    0x8f8e8d8c, 0x03020100, 0x07060504, 0x0b0a0908, /* shl  4 (16 -12)/shr12*/
    0x008f8e8d, 0x04030201, 0x08070605, 0x0c0b0a09, /* shl  3 (16 -13)/shr13*/
    0x01008f8e, 0x05040302, 0x09080706, 0x0d0c0b0a, /* shl  2 (16 -14)/shr14*/
    0x0201008f, 0x06050403, 0x0a090807, 0x0e0d0c0b  /* shl  1 (16 -15)/shr15*/
};

static void partial_fold(const size_t len, __m128i *xmm_crc0, __m128i *xmm_crc1,
        __m128i *xmm_crc2, __m128i *xmm_crc3, __m128i *xmm_crc_part) {

    const __m128i xmm_mask3 = _mm_set1_epi32(0x80808080);

    __m128i xmm_shl, xmm_shr, xmm_tmp1, xmm_tmp2, xmm_tmp3;
    __m128i xmm_a0_0;

    xmm_shl = _mm_load_si128((__m128i *)pshufb_shf_table + (len - 1));
    xmm_shr = xmm_shl;
    xmm_shr = _mm_xor_si128(xmm_shr, xmm_mask3);

    xmm_a0_0 = _mm_shuffle_epi8(*xmm_crc0, xmm_shl);

    *xmm_crc0 = _mm_shuffle_epi8(*xmm_crc0, xmm_shr);
    xmm_tmp1 = _mm_shuffle_epi8(*xmm_crc1, xmm_shl);
    *xmm_crc0 = _mm_or_si128(*xmm_crc0, xmm_tmp1);

    *xmm_crc1 = _mm_shuffle_epi8(*xmm_crc1, xmm_shr);
    xmm_tmp2 = _mm_shuffle_epi8(*xmm_crc2, xmm_shl);
    *xmm_crc1 = _mm_or_si128(*xmm_crc1, xmm_tmp2);

    *xmm_crc2 = _mm_shuffle_epi8(*xmm_crc2, xmm_shr);
    xmm_tmp3 = _mm_shuffle_epi8(*xmm_crc3, xmm_shl);
    *xmm_crc2 = _mm_or_si128(*xmm_crc2, xmm_tmp3);

    *xmm_crc3 = _mm_shuffle_epi8(*xmm_crc3, xmm_shr);
    *xmm_crc_part = _mm_shuffle_epi8(*xmm_crc_part, xmm_shl);
    *xmm_crc3 = _mm_or_si128(*xmm_crc3, *xmm_crc_part);

#ifdef ENABLE_AVX512
    *xmm_crc3 = do_one_fold_merge(xmm_a0_0, *xmm_crc3);
#else
    *xmm_crc3 = fold_xor(
      do_one_fold(xmm_a0_0),
      *xmm_crc3
    );
#endif
}

ALIGN_TO(16, static const unsigned crc_k[]) = {
    0xccaa009e, 0x00000000, /* rk1 */
    0x751997d0, 0x00000001, /* rk2 */
    0xccaa009e, 0x00000000, /* rk5 */
    0x63cd6124, 0x00000001, /* rk6 */
    0xf7011641, 0x00000000, /* rk7 */
    0xdb710640, 0x00000001  /* rk8 */
};

ALIGN_TO(16, static const unsigned crc_mask[4]) = {
    0x00000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF
};

static __m128i reverse_bits_epi8(__m128i src) {
#if defined(__GFNI__) && defined(YENC_BUILD_NATIVE) && YENC_BUILD_NATIVE!=0
    return _mm_gf2p8affine_epi64_epi8(src, _mm_set_epi32(
      0x80402010, 0x08040201,
      0x80402010, 0x08040201
    ), 0);
#else
    __m128i xmm_t0 = _mm_and_si128(src, _mm_set1_epi8(0x0f));
    __m128i xmm_t1 = _mm_and_si128(_mm_srli_epi16(src, 4), _mm_set1_epi8(0x0f));
    xmm_t0 = _mm_shuffle_epi8(_mm_set_epi8(
      -16, 112, -80, 48, -48, 80, -112, 16, -32, 96, -96, 32, -64, 64, -128, 0
      //0xf0, 0x70, 0xb0, 0x30, 0xd0, 0x50, 0x90, 0x10, 0xe0, 0x60, 0xa0, 0x20, 0xc0, 0x40, 0x80, 0
    ), xmm_t0);
    xmm_t1 = _mm_shuffle_epi8(_mm_set_epi8(
      15, 7, 11, 3, 13, 5, 9, 1, 14, 6, 10, 2, 12, 4, 8, 0
    ), xmm_t1);
    return _mm_or_si128(xmm_t0, xmm_t1);
#endif
}

#ifdef _MSC_VER
// because MSVC doesn't use BSWAP unless you specifically tell it to...
# include <stdlib.h>
# define BSWAP32 _byteswap_ulong
#else
# define BSWAP32(n) ((((n)&0xff)<<24) | (((n)&0xff00)<<8) | (((n)&0xff0000)>>8) | (((n)&0xff000000)>>24))
#endif

static uint32_t crc_fold(const unsigned char *src, long len, uint32_t initial) {
    unsigned long algn_diff;
    __m128i xmm_t0, xmm_t1, xmm_t2, xmm_t3;

    // TODO: consider calculating this via a LUT instead (probably faster)
    // info from https://www.reddit.com/r/ReverseEngineering/comments/2zwhl3/mystery_constant_0x9db42487_in_intels_crc32ieee/
    // firstly, calculate: xmm_crc0 = (intial * 0x487b9c8a) mod 0x104c11db7, where 0x487b9c8a = inverse(1<<512) mod 0x104c11db7

    // reverse input bits + load into XMM register
    uint32_t init_t = BSWAP32(initial);
    xmm_t0 = reverse_bits_epi8(_mm_cvtsi32_si128(~init_t));

    xmm_t0 = _mm_clmulepi64_si128(xmm_t0, _mm_cvtsi32_si128(0x487b9c8a), 0);
    xmm_t1 = _mm_and_si128(xmm_t0, _mm_set_epi32(-1,-1,-1,0)); // shifted up by 32bits to avoid shifts by using clmul's capability to select top 64bits instead
    xmm_t2 = _mm_set_epi32( // polynomial reduction factors
      0, 0x04c11db7, // G*
      1, 0x04d101df  // Q+
    );
    xmm_t1 = _mm_clmulepi64_si128(xmm_t1, xmm_t2, 0);
    xmm_t1 = _mm_clmulepi64_si128(xmm_t1, xmm_t2, 0x11);

    __m128i xmm_crc0 = _mm_xor_si128(xmm_t0, xmm_t1);
    // reverse bits
    xmm_crc0 = _mm_shuffle_epi8(reverse_bits_epi8(xmm_crc0), _mm_set_epi32(-1,-1,-1,0x00010203));

    __m128i xmm_crc1 = _mm_setzero_si128();
    __m128i xmm_crc2 = _mm_setzero_si128();
    __m128i xmm_crc3 = _mm_setzero_si128();
    __m128i xmm_crc_part;

    if (len < 16) {
        if (len == 0)
            return initial;
        xmm_crc_part = _mm_loadu_si128((__m128i *)src);
        goto partial;
    }

    algn_diff = (0 - (uintptr_t)src) & 0xF;
    if (algn_diff) {
        xmm_crc_part = _mm_loadu_si128((__m128i *)src);

        src += algn_diff;
        len -= algn_diff;

        partial_fold(algn_diff, &xmm_crc0, &xmm_crc1, &xmm_crc2, &xmm_crc3,
            &xmm_crc_part);
    }

    while ((len -= 64) >= 0) {
        xmm_t0 = _mm_load_si128((__m128i *)src);
        xmm_t1 = _mm_load_si128((__m128i *)src + 1);
        xmm_t2 = _mm_load_si128((__m128i *)src + 2);
        xmm_t3 = _mm_load_si128((__m128i *)src + 3);

#ifdef ENABLE_AVX512
        xmm_crc0 = do_one_fold_merge(xmm_crc0, xmm_t0);
        xmm_crc1 = do_one_fold_merge(xmm_crc1, xmm_t1);
        xmm_crc2 = do_one_fold_merge(xmm_crc2, xmm_t2);
        xmm_crc3 = do_one_fold_merge(xmm_crc3, xmm_t3);
#else
        // nesting do_one_fold() in _mm_xor_si128() seems to cause MSVC to generate horrible code, so separate it out
        xmm_crc0 = do_one_fold(xmm_crc0);
        xmm_crc1 = do_one_fold(xmm_crc1);
        xmm_crc2 = do_one_fold(xmm_crc2);
        xmm_crc3 = do_one_fold(xmm_crc3);
        xmm_crc0 = _mm_xor_si128(xmm_crc0, xmm_t0);
        xmm_crc1 = _mm_xor_si128(xmm_crc1, xmm_t1);
        xmm_crc2 = _mm_xor_si128(xmm_crc2, xmm_t2);
        xmm_crc3 = _mm_xor_si128(xmm_crc3, xmm_t3);
#endif

        src += 64;
    }

    /*
     * len = num bytes left - 64
     */
    if (len + 16 >= 0) {
        len += 16;

        xmm_t0 = _mm_load_si128((__m128i *)src);
        xmm_t1 = _mm_load_si128((__m128i *)src + 1);
        xmm_t2 = _mm_load_si128((__m128i *)src + 2);

        xmm_t3 = xmm_crc3;
#ifdef ENABLE_AVX512
        xmm_crc3 = do_one_fold_merge(xmm_crc2, xmm_t2);
        xmm_crc2 = do_one_fold_merge(xmm_crc1, xmm_t1);
        xmm_crc1 = do_one_fold_merge(xmm_crc0, xmm_t0);
#else
        xmm_crc3 = do_one_fold(xmm_crc2);
        xmm_crc2 = do_one_fold(xmm_crc1);
        xmm_crc1 = do_one_fold(xmm_crc0);
        xmm_crc3 = _mm_xor_si128(xmm_crc3, xmm_t2);
        xmm_crc2 = _mm_xor_si128(xmm_crc2, xmm_t1);
        xmm_crc1 = _mm_xor_si128(xmm_crc1, xmm_t0);
#endif
        xmm_crc0 = xmm_t3;

        if (len == 0)
            goto done;

        xmm_crc_part = _mm_load_si128((__m128i *)src + 3);
    } else if (len + 32 >= 0) {
        len += 32;

        xmm_t0 = _mm_load_si128((__m128i *)src);
        xmm_t1 = _mm_load_si128((__m128i *)src + 1);

        xmm_t2 = xmm_crc2;
        xmm_t3 = xmm_crc3;
#ifdef ENABLE_AVX512
        xmm_crc3 = do_one_fold_merge(xmm_crc1, xmm_t1);
        xmm_crc2 = do_one_fold_merge(xmm_crc0, xmm_t0);
#else
        xmm_crc3 = do_one_fold(xmm_crc1);
        xmm_crc2 = do_one_fold(xmm_crc0);
        xmm_crc3 = _mm_xor_si128(xmm_crc3, xmm_t1);
        xmm_crc2 = _mm_xor_si128(xmm_crc2, xmm_t0);
#endif
        xmm_crc1 = xmm_t3;
        xmm_crc0 = xmm_t2;

        if (len == 0)
            goto done;

        xmm_crc_part = _mm_load_si128((__m128i *)src + 2);
    } else if (len + 48 >= 0) {
        len += 48;

        xmm_t0 = _mm_load_si128((__m128i *)src);

        xmm_t3 = xmm_crc3;
#ifdef ENABLE_AVX512
        xmm_crc3 = do_one_fold_merge(xmm_crc0, xmm_t0);
#else
        xmm_crc3 = _mm_xor_si128(do_one_fold(xmm_crc0), xmm_t0);
#endif
        xmm_crc0 = xmm_crc1;
        xmm_crc1 = xmm_crc2;
        xmm_crc2 = xmm_t3;

        if (len == 0)
            goto done;

        xmm_crc_part = _mm_load_si128((__m128i *)src + 1);
    } else {
        len += 64;
        if (len == 0)
            goto done;
        xmm_crc_part = _mm_load_si128((__m128i *)src);
    }

partial:
    partial_fold(len, &xmm_crc0, &xmm_crc1, &xmm_crc2, &xmm_crc3,
        &xmm_crc_part);
done:
{
    const __m128i xmm_mask = _mm_load_si128((__m128i *)crc_mask);
    __m128i x_tmp0, x_tmp1, x_tmp2, crc_fold;

    /*
     * k1
     */
    crc_fold = _mm_load_si128((__m128i *)crc_k);

    x_tmp0 = _mm_clmulepi64_si128(xmm_crc0, crc_fold, 0x10);
    xmm_crc0 = _mm_clmulepi64_si128(xmm_crc0, crc_fold, 0x01);
#ifdef ENABLE_AVX512
    xmm_crc1 = _mm_ternarylogic_epi32(xmm_crc1, x_tmp0, xmm_crc0, 0x96);
#else
    xmm_crc1 = _mm_xor_si128(xmm_crc1, x_tmp0);
    xmm_crc1 = _mm_xor_si128(xmm_crc1, xmm_crc0);
#endif

    x_tmp1 = _mm_clmulepi64_si128(xmm_crc1, crc_fold, 0x10);
    xmm_crc1 = _mm_clmulepi64_si128(xmm_crc1, crc_fold, 0x01);
#ifdef ENABLE_AVX512
    xmm_crc2 = _mm_ternarylogic_epi32(xmm_crc2, x_tmp1, xmm_crc1, 0x96);
#else
    xmm_crc2 = _mm_xor_si128(xmm_crc2, x_tmp1);
    xmm_crc2 = _mm_xor_si128(xmm_crc2, xmm_crc1);
#endif

    x_tmp2 = _mm_clmulepi64_si128(xmm_crc2, crc_fold, 0x10);
    xmm_crc2 = _mm_clmulepi64_si128(xmm_crc2, crc_fold, 0x01);
#ifdef ENABLE_AVX512
    xmm_crc3 = _mm_ternarylogic_epi32(xmm_crc3, x_tmp2, xmm_crc2, 0x96);
#else
    xmm_crc3 = _mm_xor_si128(xmm_crc3, x_tmp2);
    xmm_crc3 = _mm_xor_si128(xmm_crc3, xmm_crc2);
#endif

    /*
     * k5
     */
    crc_fold = _mm_load_si128((__m128i *)crc_k + 1);

    xmm_crc0 = xmm_crc3;
    xmm_crc3 = _mm_clmulepi64_si128(xmm_crc3, crc_fold, 0);
    xmm_crc0 = _mm_srli_si128(xmm_crc0, 8);
    xmm_crc3 = _mm_xor_si128(xmm_crc3, xmm_crc0);

    xmm_crc0 = xmm_crc3;
    xmm_crc3 = _mm_slli_si128(xmm_crc3, 4);
    xmm_crc3 = _mm_clmulepi64_si128(xmm_crc3, crc_fold, 0x10);
#ifdef ENABLE_AVX512
    //xmm_crc3 = _mm_maskz_xor_epi32(14, xmm_crc3, xmm_crc0);
    xmm_crc3 = _mm_ternarylogic_epi32(xmm_crc3, xmm_crc0, xmm_mask, 0x28);
#else
    xmm_crc0 = _mm_and_si128(xmm_crc0, xmm_mask);
    xmm_crc3 = _mm_xor_si128(xmm_crc3, xmm_crc0);
#endif

    /*
     * k7
     */
    xmm_crc1 = xmm_crc3;
    crc_fold = _mm_load_si128((__m128i *)crc_k + 2);

    xmm_crc3 = _mm_clmulepi64_si128(xmm_crc3, crc_fold, 0);
    xmm_crc3 = _mm_clmulepi64_si128(xmm_crc3, crc_fold, 0x10);
#ifdef ENABLE_AVX512
    xmm_crc3 = _mm_ternarylogic_epi32(xmm_crc3, xmm_crc1, xmm_crc1, 0xC3); // NOT(xmm_crc3 ^ xmm_crc1)
#else
    xmm_crc1 = _mm_xor_si128(xmm_crc1, xmm_mask);
    xmm_crc3 = _mm_xor_si128(xmm_crc3, xmm_crc1);
#endif
    return _mm_extract_epi32(xmm_crc3, 2);
}

}

static uint32_t do_crc32_incremental_clmul(const void* data, size_t length, uint32_t init) {
    return crc_fold((const unsigned char*)data, (long)length, init);
}

void crc_clmul_set_funcs(crc_func* _do_crc32_incremental) {
    *_do_crc32_incremental = &do_crc32_incremental_clmul;
}
#else

void crc_clmul_set_funcs(crc_func *_do_crc32_incremental) {
    (void) _do_crc32_incremental;
}

#endif

