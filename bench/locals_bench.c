// 22.1: how long the compiler takes to find a local, with the book's linear
// scan (the default) and with -DLOCAL_HASH. Each program declares some locals
// in a block, then uses random ones 20,000 times. The time for the program
// with no uses is subtracted, so what's left is the cost of the uses.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/chunk.h"
#include "../src/compiler.h"
#include "../src/vm.h"

#define USES 20000

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}

static int compareDoubles(const void* a, const void* b) {
  double x = *(const double*)a, y = *(const double*)b;
  return (x > y) - (x < y);
}

// The time to compile source once.
static double compileOnce(const char* source) {
  Chunk chunk;
  initChunk(&chunk);
  double start = now();
  if (!compile(source, &chunk)) exit(1);
  double time = now() - start;
  freeChunk(&chunk);
  return time;
}

// The median time to compile source, in ns. Small programs are compiled many
// times per measurement, since the clock only counts microseconds.
static double compileTime(const char* source) {
  int reps = 1 + (int)(2e6 / (compileOnce(source) + 1));
  double times[7];
  for (int run = 0; run < 7; run++) {
    double start = now();
    for (int i = 0; i < reps; i++) compileOnce(source);
    times[run] = (now() - start) / reps;
  }
  qsort(times, 7, sizeof(double), compareDoubles);
  return times[3];
}

static char* program(int locals, int uses) {
  char* source = malloc((size_t)(locals + uses) * 16 + 16);
  char* end = source;
  end += sprintf(end, "{\n");
  for (int i = 0; i < locals; i++) end += sprintf(end, "var v%d;\n", i);

  uint32_t random = 12345;
  for (int i = 0; i < uses; i++) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    end += sprintf(end, "v%d;\n", (int)(random % locals));
  }
  sprintf(end, "}\n");
  return source;
}

int main(void) {
  initVM();
  int sizes[] = {1, 4, 16, 64, 256, 1024, 4096};
  for (int i = 0; i < 7; i++) {
    char* declarations = program(sizes[i], 0);
    char* full = program(sizes[i], USES);
    double base = compileTime(declarations);
    double total = compileTime(full);
    printf("  %4d locals: %6.1f ns per use, %6.1f ns per declaration\n",
           sizes[i], (total - base) / USES, base / sizes[i]);
    free(declarations);
    free(full);
  }
  freeVM();
  return 0;
}
