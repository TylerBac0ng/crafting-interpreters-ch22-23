#ifndef clox_vm_h
#define clox_vm_h

#include "chunk.h"
#include "table.h"
#include "value.h"

typedef struct {
  Chunk* chunk;
  uint8_t* ip;
  // 22.4: the book's stack holds 256 values, which isn't enough for more
  // than 256 locals. Like my 15.3, it is a dynamic array that grows.
  Value* stack;
  Value* stackTop;
  int stackCapacity;
  // 22.3: the names of global constants, so later lines of the REPL can't
  // assign to them either.
  Table constGlobals;
#ifdef GLOBAL_TABLE
  Table globals;
#else
  // 21.2: the compiler gives each global name a slot. globalNames maps the
  // name to its slot number, and globalValues holds the values, with
  // EMPTY_VAL in a slot that isn't defined yet. globalIdentifiers has the
  // name for each slot, for error messages.
  Table globalNames;
  ValueArray globalValues;
  ValueArray globalIdentifiers;
#endif
  Table strings;
  Obj* objects;
#ifdef CONSTANT_STRINGS
  struct KeptSource* sources;
#endif
} VM;

typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR
} InterpretResult;

extern VM vm;

void initVM();
void freeVM();
InterpretResult interpret(const char* source);
InterpretResult runChunk(Chunk* chunk);
// 19.2: takes ownership of a heap allocated source string and frees it in
// freeVM(), since constant strings can point into it.
void keepSource(char* source);
void push(Value value);
Value pop();

#endif
