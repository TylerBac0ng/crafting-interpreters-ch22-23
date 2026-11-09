// Chapter 22: local variables, constants (22.3) and more than 256 locals
// (22.4). Runs in every build, including LOCAL_HASH (22.1), which has to
// give the same results as the linear scan.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/chunk.h"
#include "../src/compiler.h"
#include "../src/debug.h"
#include "../src/vm.h"

// Runs a program, then prints how many values it left on the stack. Every
// program should leave 0.
static void run(const char* source) {
  interpret(source);
  printf("stack: %d\n", (int)(vm.stackTop - vm.stack));
}

// Appends to a growing source string.
static char* source;
static int length;
static int capacity;
static void add(const char* format, int a, int b) {
  if (length + 64 > capacity) {
    capacity = capacity * 2 + 1024;
    source = realloc(source, capacity);
  }
  length += snprintf(source + length, capacity - length, format, a, b);
}
static void reset() {
  length = 0;
  if (source != NULL) source[0] = '\0';
}

// How many times an instruction appears in the code for source.
static int countInstruction(const char* program, OpCode op) {
  Chunk chunk;
  initChunk(&chunk);
  int count = -1;
  if (compile(program, &chunk)) {
    count = 0;
    for (int offset = 0; offset < chunk.count;) {
      uint8_t instruction = chunk.code[offset];
      if (instruction == op) count++;
      switch (instruction) {
        case OP_GET_LOCAL_LONG: case OP_SET_LOCAL_LONG:
        case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_LOOP:
#ifndef GLOBAL_TABLE
        case OP_DEFINE_GLOBAL: case OP_GET_GLOBAL: case OP_SET_GLOBAL:
#endif
          offset += 3; break;
        case OP_CONSTANT: case OP_GET_LOCAL: case OP_SET_LOCAL:
#ifdef GLOBAL_TABLE
        case OP_DEFINE_GLOBAL: case OP_GET_GLOBAL: case OP_SET_GLOBAL:
#endif
          offset += 2; break;
        default: offset += 1; break;
      }
    }
  }
  freeChunk(&chunk);
  return count;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  initVM();

  printf("-- the book's locals\n");
  run("{ var a = \"outer\"; { var a = \"inner\"; print a; } print a; }");
  run("{ var a = 1; var b = 2; a = b = a + b; print a; print b; }");
  run("var g = \"global\"; { var g = \"local\"; print g; } print g;");
  run("{ var a; print a; }");
  run("{ var a = 1; var a = 2; }");
  run("{ var a = \"outer\"; { var a = a; } }");
  run("{ var a = 1; { var a = 2; { var a = 3; print a; } print a; } print a; }");

  printf("-- 22.3: const\n");
  run("{ const a = 1; print a; }");
  run("{ const a = 1; a = 2; }");
  run("{ const a; }");
  run("{ const a = 1; { var a = 2; a = 3; print a; } print a; }");
  run("{ var a = 1; { const a = 2; print a; } a = 3; print a; }");
  run("const pi = 3.14; print pi;");
  run("pi = 3;");
  run("var pi = 3;");
  run("const pi = 3;");
  run("print pi;");
  // A const in code that doesn't compile isn't recorded.
  run("const e = 2.71; print oops oops;");
  run("var e = \"not constant\"; e = \"changed\"; print e;");

  printf("-- 22.4: more than 256 locals\n");
  reset();
  add("{\n", 0, 0);
  // No initializers, since 300 number constants would hit the constant limit.
  for (int i = 0; i < 300; i++) add("  var v%d;\n", i, 0);
  add("  v0 = 0; v255 = 255; v256 = 256; v299 = 299; v298 = \"v298\";\n", 0, 0);
  add("  print v0; print v1; print v255; print v256; print v299;\n", 0, 0);
  add("  v299 = v0 + v299 + 1; print v299;\n", 0, 0);
  add("  { var inner = v298; print inner; }\n", 0, 0);
  add("}\n", 0, 0);
  run(source);
  printf("OP_GET_LOCAL_LONG: %d\n",
         countInstruction(source, OP_GET_LOCAL_LONG));
  printf("OP_SET_LOCAL_LONG: %d\n",
         countInstruction(source, OP_SET_LOCAL_LONG));

  // The limit is now 65,536. 256 nested blocks of 256 locals reach it, and
  // one more is an error.
  for (int extra = 0; extra <= 1; extra++) {
    reset();
    for (int block = 0; block < 256; block++) {
      add("{", 0, 0);
      for (int i = 0; i < 256; i++) add("var v%d_%d;", block, i);
    }
    if (extra) add("{ var oneTooMany; }", 0, 0);
    add("print v255_255;", 0, 0);
    for (int block = 0; block < 256; block++) add("}", 0, 0);
    run(source);
  }

  free(source);
  freeVM();
  return 0;
}
