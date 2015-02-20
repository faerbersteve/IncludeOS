#include <utility/memstream.h>

#include <x86intrin.h>

void* aligned_alloc(size_t n, size_t ALIGNMENT)
{
  // allocate aligned address + space for pointer
  void* mem = malloc(n + (ALIGNMENT-1) + sizeof(void*));
  if (!mem) return mem;
  
  // calculate placement for aligned memory
  intptr_t addr = ((intptr_t) mem + sizeof(void*) + (ALIGNMENT-1)) & ~(ALIGNMENT-1);
  void* ptr = (void*) addr;
  // remember the malloc() result
  ((void**)ptr)[-1] = mem;
  // return pointer to aligned memory
  return ptr;
}
void aligned_free(void* ptr)
{
  // free all memory by freeing the malloc() result
  if (ptr) free(((void**) ptr)[-1]);
}

void* streamcpy(void* dest, const void* srce, size_t n)
{
  char* dst       = (char*) dest;
  const char* src = (const char*) srce;
  
  // copy up to 15 bytes until SSE-aligned
  while (((intptr_t) dst & (SSE_SIZE-1)) && n)
  {
    *dst++ = *src++; n--;
  }
  // copy SSE-aligned
  while (n >= SSE_SIZE)
  {
    __m128i data = _mm_load_si128((__m128i*) src);
    _mm_stream_si128((__m128i*) dst, data);
    
    dst += SSE_SIZE;
    src += SSE_SIZE;
    
    n -= SSE_SIZE;
  }
  // copy remainder
  while (n--)
  {
    *dst++ = *src++;
  }
  return dst;
}
void* streamucpy(void* dest, const void* usrc, size_t n)
{
  char* dst       = (char*) dest;
  const char* src = (const char*) usrc;
  
  // copy up to 15 bytes until SSE-aligned
  while (((intptr_t) dst & (SSE_SIZE-1)) && n)
  {
    *dst++ = *src++; n--;
  }
  // copy SSE-aligned
  while (n >= SSE_SIZE)
  {
    __m128i data = _mm_loadu_si128((__m128i*) src);
    _mm_stream_si128((__m128i*) dst, data);
    
    dst  += SSE_SIZE;
    src += SSE_SIZE;
    
    n -= SSE_SIZE;
  }
  // copy remainder
  while (n--)
  {
    *dst++ = *src++;
  }
  return dst;
}

inline char* stream_fill(char* dst, size_t* n, const __m128i data)
{
  while (*n >= SSE_SIZE)
  {
    _mm_stream_si128((__m128i*) dst, data);
    
    dst += SSE_SIZE;
    *n  -= SSE_SIZE;
  }
  return dst;
}

void* streamset8(void* dest, int8_t value, size_t n)
{
  char* dst = dest;
  
  // memset up to 15 bytes until SSE-aligned
  while (((intptr_t) dst & (SSE_SIZE-1)) && n)
  {
    *dst++ = value; n--;
  }
  // memset SSE-aligned
  const __m128i data = _mm_set1_epi8(value);
  dst = stream_fill(dst, &n, data);
  // memset remainder
  while (n--)
  {
    *dst++ = value;
  }
  return dst;
}
void* streamset16(void* dest, int16_t value, size_t n)
{
  char* dst = dest;
  
  // memset up to 15 bytes until SSE-aligned
  while (((intptr_t) dst & (SSE_SIZE-1)) && n)
  {
    *dst++ = value;
    n -= sizeof(int16_t);
  }
  // memset SSE-aligned
  const __m128i data = _mm_set1_epi16(value);
  dst = stream_fill(dst, &n, data);
  // memset remainder
  while (n)
  {
    *dst++ = value;
    n -= sizeof(int16_t);
  }
  return dst;
}
void* streamset32(void* dest, int32_t value, size_t n)
{
  char* dst = dest;
  
  // memset up to 15 bytes until SSE-aligned
  while (((intptr_t) dst & (SSE_SIZE-1)) && n)
  {
    *dst++ = value;
    n -= sizeof(int32_t);
  }
  // memset SSE-aligned
  const __m128i data = _mm_set1_epi32(value);
  dst = stream_fill(dst, &n, data);
  // memset remainder
  while (n)
  {
    *dst++ = value;
    n -= sizeof(int32_t);
  }
  return dst;
}