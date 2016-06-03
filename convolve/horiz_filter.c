#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tmmintrin.h>

static inline unsigned int readtsc(void) {
  unsigned int tsc;
  __asm__ __volatile__("rdtsc\n\t":"=a"(tsc):);
  return tsc;
}

#define FILTER_BITS         (7)
#define TEST_NUM            (32)
#define HD_WIDTH            (1920)
#define SUPER_BLOCK_HEIGHT  (128)
#define RAND_SEED           (0xabc)
unsigned int seed = RAND_SEED;

int round_power_of_two(int x, int n) {
  int ret = (x + (1 << (n - 1))) >> n;
  return ret;
}

uint8_t inline clip_pixel(int x) {
  uint8_t ret = x;
  if (x < 0) {
    ret = 0;
  }
  if (x > 255) {
    ret = 255;
  }
  return ret;
}

static int filtering(const uint8_t *src, const int16_t *filter, int flen) {
  int k;
  int sum = 0;
  int prod;
  for (k = 0; k < flen; ++k) {
    prod = src[k] * filter[k];
    sum += prod;
  }
  return sum;
}

void convolve(const uint8_t *src, int w, const int16_t *filter, int flen,
                uint8_t *buffer) {
  int i;
  int sum;

  for (i = 0; i < w; ++i) {
    sum = filtering(src, filter, flen);
    buffer[i] = clip_pixel(round_power_of_two(sum, FILTER_BITS));
    src += 1;
  }
}

void init_state(uint8_t *buf, uint8_t *pixel, const unsigned int random,
                int width, int height) {
  int row, col;
  int block = width * height;

  memset(buf, 0, sizeof(buf[0]) * block);
  memset(pixel, 0, sizeof(pixel[0]) * block);

  seed = random;
  for (row = 0; row < height; ++row) {
    for (col = 0; col < width; ++col) {
      pixel[col] = clip_pixel(rand_r(&seed) % 255);
    }
    pixel += width;
  }
}

void check_buffer(const uint8_t *buf1, const uint8_t *buf2,
                  int width, int height) {
  int row, col;
  for (row = 0; row < height; ++row) {
    for (col = 0; col < width; ++col) {
      if (buf1[col] != buf2[col]) {
        printf("Not bit-exact on index %d\n", col);
        printf("Expected: 0x%x, Actual: 0x%x\n", buf1[col], buf2[col]);
        return;
      }
    }
    buf1 += width;
    buf2 += width;
  }
}

static const int16_t filter12[12] = {
  -1,   3,  -4,   8, -18, 120,  28, -12,   7,  -4,   2, -1};

static const int16_t filter10[10] = {
  1,  -3,   7, -17, 119,  28, -11,   5,  -2, 1};

// SSSE3

const int8_t pfilter12[2][16] __attribute__ ((aligned(16))) = {
  {-1,  3, -4,  8, -18, 120,  28, -12,   7,  -4,   2,  -1,  0,  0,  0,  0},
  { 0,  0, -1,  3,  -4,   8, -18, 120,  28, -12,   7,  -4,  2, -1,  0,  0},
};

const int8_t pfilter10[2][16] __attribute__ ((aligned(16))) = {
  {0, 1,  -3,   7, -17, 119,  28, -11,   5,  -2, 1, 0, 0, 0, 0, 0},
  {0, 0, 0, 1,  -3,   7, -17, 119,  28, -11,   5,  -2, 1, 0, 0, 0},
};

struct Filter {
  const int8_t (*coeffs)[16];
  int tapsNum;
  int signalSpan;
};

const struct Filter pfilter_12tap = {
  pfilter12, 12, 5
};

const struct Filter pfilter_10tap = {
  pfilter10, 10, 7
};

void inline transpose_4x8(const __m128i *in, __m128i *out) {
  __m128i t0, t1;

  t0 = _mm_unpacklo_epi16(in[0], in[1]);
  t1 = _mm_unpacklo_epi16(in[2], in[3]);

  out[0] = _mm_unpacklo_epi32(t0, t1);
  out[1] = _mm_srli_si128(out[0], 8);
  out[2] = _mm_unpackhi_epi32(t0, t1);
  out[3] = _mm_srli_si128(out[2], 8);

  t0 = _mm_unpackhi_epi16(in[0], in[1]);
  t1 = _mm_unpackhi_epi16(in[2], in[3]);

  out[4] = _mm_unpacklo_epi32(t0, t1);
  out[5] = _mm_srli_si128(out[4], 8);
  // Note: We ignore out[6] and out[7] because
  // they're zero vectors.
}

void horiz_w4_ssse3(const uint8_t *src, const __m128i *f,
                    int tapsNum, uint8_t *buffer) {
  __m128i sumPairRow[4];
  __m128i sumPairCol[8];
  __m128i pixel;
  const __m128i k_256 = _mm_set1_epi16(1 << 8);

  if (10 == tapsNum) {
    src -= 1;
  }

  pixel = _mm_loadu_si128((__m128i const *)src);
  sumPairRow[0] = _mm_maddubs_epi16(pixel, f[0]);
  sumPairRow[2] = _mm_maddubs_epi16(pixel, f[1]);
  sumPairRow[2] = _mm_srli_si128(sumPairRow[2], 2);

  pixel = _mm_loadu_si128((__m128i const *)(src + 1));
  sumPairRow[1] = _mm_maddubs_epi16(pixel, f[0]);
  sumPairRow[3] = _mm_maddubs_epi16(pixel, f[1]);
  sumPairRow[3] = _mm_srli_si128(sumPairRow[3], 2);

  transpose_4x8(sumPairRow, sumPairCol);

  sumPairRow[0] = _mm_adds_epi16(sumPairCol[0], sumPairCol[1]);
  sumPairRow[1] = _mm_adds_epi16(sumPairCol[4], sumPairCol[5]);

  sumPairRow[2] = _mm_min_epi16(sumPairCol[2], sumPairCol[3]);
  sumPairRow[3] = _mm_max_epi16(sumPairCol[2], sumPairCol[3]);

  sumPairRow[0] = _mm_adds_epi16(sumPairRow[0], sumPairRow[1]);
  sumPairRow[0] = _mm_adds_epi16(sumPairRow[0], sumPairRow[2]);
  sumPairRow[0] = _mm_adds_epi16(sumPairRow[0], sumPairRow[3]);

  sumPairRow[1] = _mm_mulhrs_epi16(sumPairRow[0], k_256);
  sumPairRow[2] = _mm_packus_epi16(sumPairRow[1], sumPairRow[1]);

  *(int *)buffer = _mm_cvtsi128_si32(sumPairRow[2]);
}

void horiz_w8_ssse3(const uint8_t *src, const __m128i *f, int tapsNum,
                    uint8_t *buf) {
  horiz_w4_ssse3(src, f, tapsNum, buf);
  src += 4;
  buf += 4;
  horiz_w4_ssse3(src, f, tapsNum, buf);
}

void horiz_w16_ssse3(const uint8_t *src, const __m128i *f, int tapsNum,
                     uint8_t *buf) {
  horiz_w8_ssse3(src, f, tapsNum, buf);
  src += 8;
  buf += 8;
  horiz_w8_ssse3(src, f, tapsNum, buf);
}

void horiz_w32_ssse3(const uint8_t *src, const __m128i *f, int tapsNum,
                     uint8_t *buf) {
  horiz_w16_ssse3(src, f, tapsNum, buf);
  src += 16;
  buf += 16;
  horiz_w16_ssse3(src, f, tapsNum, buf);
}

void horiz_w64_ssse3(const uint8_t *src, const __m128i *f, int tapsNum,
                     uint8_t *buf) {
  horiz_w32_ssse3(src, f, tapsNum, buf);
  src += 32;
  buf += 32;
  horiz_w32_ssse3(src, f, tapsNum, buf);
}

void (*horizTab[5])(const uint8_t *, const __m128i *, int, uint8_t *) = {
   horiz_w4_ssse3,
   horiz_w8_ssse3,
   horiz_w16_ssse3,
   horiz_w32_ssse3,
   horiz_w64_ssse3,
};

void horiz_filter_ssse3(const uint8_t *src, const struct Filter fData,
                        int width, uint8_t *buffer) {
  const int16_t *filter = (const int16_t *) fData.coeffs;
  __m128i f[2];

  f[0] = *((__m128i *)(fData.coeffs));
  f[1] = *((__m128i *)(fData.coeffs + 1));

  switch (width) {
    case 4:
      horizTab[0](src, f, fData.tapsNum, buffer);
      break;
    case 8:
      horizTab[1](src, f, fData.tapsNum, buffer);
      break;
    case 16:
      horizTab[2](src, f, fData.tapsNum, buffer);
      break;
    case 32:
      horizTab[3](src, f, fData.tapsNum, buffer);
      break;
    case 64:
      horizTab[4](src, f, fData.tapsNum, buffer);
      break;
    default:
      assert(0);
  }
}

// Testing wrapper functions

void run_prototype_filter(uint8_t *src, int width, int height,
                          const int16_t *filter, int flen, uint8_t *dst) {
  uint32_t start, end;
  int count = 0;

  start = readtsc();
  do {
    convolve(src, width, filter, flen, dst);
    src += width;
    dst += width;
    count++;
  } while (count < height);
  end = readtsc();

  printf("C version cycles:\t%d\n", end - start);
}

void run_target_filter(uint8_t *src, struct Filter filter,
                       int width, int height, uint8_t *dst) {
  uint32_t start, end;
  int count = 0;

  start = readtsc();
  do {
    horiz_filter_ssse3(src, filter, width, dst);
    src += width;
    dst += width;
    count++;
  } while (count < height);
  end = readtsc();

  printf("SIMD version cycles:\t%d\n", end - start);
}

int main(int argc, char **argv)
{
  // We simulate HD width (1920) and max super block height (128)
  const size_t block_size = HD_WIDTH * SUPER_BLOCK_HEIGHT;

  if (argc != 3) {
    printf("Usage: filtering <seed> <width>\n");
    printf("width = 4, 8, 16, 32, 64. seed = random seed number.\n");
    return -1;
  }

  const int width = atoi(argv[2]);
  const unsigned int random_seed = atoi(argv[1]);
  const int height = 8;

  uint8_t *buffer = (uint8_t *) malloc(2 * sizeof(buffer[0]) * block_size);
  uint8_t *pixel = (uint8_t *) malloc(2 * sizeof(pixel[0]) * block_size);
  uint8_t *ppixel = pixel + block_size;
  uint8_t *pbuffer = buffer + block_size;

  init_state(buffer, pixel, random_seed, width, height);
  init_state(pbuffer, ppixel, random_seed, width, height);

  run_prototype_filter(pixel, width, height, filter12, 12, buffer);
  run_target_filter(ppixel, pfilter_12tap, width, height, pbuffer);
  check_buffer(buffer, pbuffer, width, height);

  run_prototype_filter(pixel, width, height, filter10, 10, buffer);
  run_target_filter(ppixel, pfilter_10tap, width, height, pbuffer);
  check_buffer(buffer, pbuffer, width, height);

  free(buffer);
  free(pixel);
  return 0;
}
