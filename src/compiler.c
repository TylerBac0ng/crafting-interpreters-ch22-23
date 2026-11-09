#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "memory.h"
#include "object.h"
#include "scanner.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct {
  Token current;
  Token previous;
  bool hadError;
  bool panicMode;
} Parser;

typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT,  // =
  PREC_CONDITIONAL, // ?:
  PREC_OR,          // or
  PREC_AND,         // and
  PREC_EQUALITY,    // == !=
  PREC_COMPARISON,  // < > <= >=
  PREC_TERM,        // + -
  PREC_FACTOR,      // * /
  PREC_UNARY,       // ! -
  PREC_CALL,        // . ()
  PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
  ParseFn prefix;
  ParseFn infix;
  Precedence precedence;
} ParseRule;

// 22.4: a local's slot can be up to two bytes, so this many can be in scope.
#define MAX_LOCALS (UINT16_MAX + 1)

typedef struct {
  Token name;
  int depth;
  bool isConst; // 22.3
#ifdef LOCAL_HASH
  // 22.1: the local with the same name that this one hides, or -1.
  int shadowed;
#endif
} Local;

// 23.2 and 23.3: the loop that a continue statement belongs to.
typedef struct Loop {
  struct Loop* enclosing;
  // Where continue jumps back to, or -1 if it jumps forward to a spot that
  // isn't compiled yet. Those jumps are kept in continueJumps.
  int start;
  // Locals deeper than this are inside the loop, so continue pops them.
  int scopeDepth;
  int* continueJumps;
  int continueCount;
  int continueCapacity;
} Loop;

typedef struct {
  // 22.4: a growable array instead of the book's fixed array of 256.
  Local* locals;
  int localCount;
  int localCapacity;
  int scopeDepth;
  Loop* loop;
#ifdef LOCAL_HASH
  // 22.1: maps each local's name to the innermost local with that name.
  Table localNames;
#endif
} Compiler;

Parser parser;
Compiler* current = NULL;
Chunk* compilingChunk;
#if defined(GLOBAL_TABLE) && !defined(NO_CONSTANT_DEDUP)
// 21.1: maps each identifier already in this chunk's constant table to its
// index.
static Table stringConstants;
#endif
// 22.3: global constants declared by the code being compiled. They are only
// added to vm.constGlobals if it compiles without errors.
static Table newConstGlobals;

// 17.1: with DEBUG_TRACE_PARSE, expression(), parsePrecedence() and the
// prefix and infix parse functions print a line when they are called,
// indented by how deeply they are nested.
#ifdef DEBUG_TRACE_PARSE
static int traceDepth = 0;

static const char* precedenceNames[] = {
  "PREC_NONE", "PREC_ASSIGNMENT", "PREC_CONDITIONAL", "PREC_OR", "PREC_AND",
  "PREC_EQUALITY", "PREC_COMPARISON", "PREC_TERM", "PREC_FACTOR",
  "PREC_UNARY", "PREC_CALL", "PREC_PRIMARY",
};

static void traceCall(const char* format, ...) {
  for (int i = 0; i < traceDepth; i++) printf("| ");
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
  printf("\n");
  traceDepth++;
}

#define TRACE_CALL(...) traceCall(__VA_ARGS__)
#define TRACE_RETURN() (traceDepth--)
#else
#define TRACE_CALL(...) ((void)0)
#define TRACE_RETURN() ((void)0)
#endif

static Chunk* currentChunk() {
  return compilingChunk;
}

static void errorAt(Token* token, const char* message) {
  if (parser.panicMode) return;
  parser.panicMode = true;
  fprintf(stderr, "[line %d] Error", token->line);

  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token->type == TOKEN_ERROR) {
    // Nothing.
  } else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }

  fprintf(stderr, ": %s\n", message);
  parser.hadError = true;
}
static void error(const char* message) {
  errorAt(&parser.previous, message);
}
static void errorAtCurrent(const char* message) {
  errorAt(&parser.current, message);
}

static void advance() {
  parser.previous = parser.current;

  for (;;) {
    parser.current = scanToken();
    if (parser.current.type != TOKEN_ERROR) break;

    errorAtCurrent(parser.current.start);
  }
}
static void consume(TokenType type, const char* message) {
  if (parser.current.type == type) {
    advance();
    return;
  }

  errorAtCurrent(message);
}
static bool check(TokenType type) {
  return parser.current.type == type;
}
static bool match(TokenType type) {
  if (!check(type)) return false;
  advance();
  return true;
}
static void emitByte(uint8_t byte) {
  writeChunk(currentChunk(), byte, parser.previous.line);
}
static void emitBytes(uint8_t byte1, uint8_t byte2) {
  emitByte(byte1);
  emitByte(byte2);
}
static void emitLoop(int loopStart) {
  emitByte(OP_LOOP);

  int offset = currentChunk()->count - loopStart + 2;
  if (offset > UINT16_MAX) error("Loop body too large.");

  emitByte((offset >> 8) & 0xff);
  emitByte(offset & 0xff);
}
static void emitReturn() {
  emitByte(OP_RETURN);
}
static uint8_t makeConstant(Value value) {
  int constant = addConstant(currentChunk(), value);
  if (constant > UINT8_MAX) {
    error("Too many constants in one chunk.");
    return 0;
  }

  return (uint8_t)constant;
}
static void emitConstant(Value value) {
  emitBytes(OP_CONSTANT, makeConstant(value));
}
static int emitJump(uint8_t instruction) {
  emitByte(instruction);
  emitByte(0xff);
  emitByte(0xff);
  return currentChunk()->count - 2;
}
static void patchJump(int offset) {
  // -2 to adjust for the bytecode for the jump offset itself.
  int jump = currentChunk()->count - offset - 2;

  if (jump > UINT16_MAX) {
    error("Too much code to jump over.");
  }

  currentChunk()->code[offset] = (jump >> 8) & 0xff;
  currentChunk()->code[offset + 1] = jump & 0xff;
}
#ifdef MINIMAL_INSTRUCTIONS
// Loads a constant the compiler made up (nil, true, false or -1), reusing
// the chunk's existing copy if it has one so they don't fill up the table.
static void emitSharedConstant(Value value) {
  ValueArray* constants = &currentChunk()->constants;
  for (int i = 0; i < constants->count && i <= UINT8_MAX; i++) {
    if (valuesEqual(constants->values[i], value)) {
      emitBytes(OP_CONSTANT, (uint8_t)i);
      return;
    }
  }

  emitConstant(value);
}
// Replaces OP_NEGATE. x * -1 is exactly -x, even for 0, where 0 - x would
// give 0 instead of -0.
static void emitNegate() {
  emitSharedConstant(NUMBER_VAL(-1));
  emitByte(OP_MULTIPLY);
}
#endif
// Loads nil. The minimal build has no OP_NIL (18.1).
static void emitNil() {
#ifdef MINIMAL_INSTRUCTIONS
  emitSharedConstant(NIL_VAL);
#else
  emitByte(OP_NIL);
#endif
}
static void initCompiler(Compiler* compiler) {
  compiler->locals = NULL;
  compiler->localCount = 0;
  compiler->localCapacity = 0;
  compiler->scopeDepth = 0;
  compiler->loop = NULL;
#ifdef LOCAL_HASH
  initTable(&compiler->localNames);
#endif
  current = compiler;
}
static void endCompiler() {
  emitReturn();
#ifdef DEBUG_PRINT_CODE
  if (!parser.hadError) {
    disassembleChunk(currentChunk(), "code");
  }
#endif
  FREE_ARRAY(Local, current->locals, current->localCapacity);
#ifdef LOCAL_HASH
  freeTable(&current->localNames);
#endif
  current = NULL;
}

static void expression();
static void statement();
static void declaration();
static ParseRule* getRule(TokenType type);
static void parsePrecedence(Precedence precedence);

#ifdef LOCAL_HASH
// 22.1: the key for a local's name in localNames. Strings are interned, so
// equal names give the same key.
static Value localKey(Token* name) {
  return OBJ_VAL(literalString(name->start, name->length));
}
#endif
// Removes the innermost local. Doesn't emit any code.
static void removeLocal() {
  current->localCount--;
#ifdef LOCAL_HASH
  Local* local = &current->locals[current->localCount];
  if (local->name.length == 0) return; // A hidden local (see switch).
  if (local->shadowed == -1) {
    tableDelete(&current->localNames, localKey(&local->name));
  } else {
    tableSet(&current->localNames, localKey(&local->name),
             NUMBER_VAL((double)local->shadowed));
  }
#endif
}
static void beginScope() {
  current->scopeDepth++;
}
static void endScope() {
  current->scopeDepth--;

  while (current->localCount > 0 &&
         current->locals[current->localCount - 1].depth >
            current->scopeDepth) {
    emitByte(OP_POP);
    removeLocal();
  }
}

#ifdef MINIMAL_INSTRUCTIONS
// 18.1: everything is built from OP_EQUAL, OP_LESS, OP_ADD, OP_MULTIPLY,
// OP_DIVIDE and OP_NOT. a > b is compiled as -a < -b, so the left operand,
// which is already on the stack, gets negated before the right one is
// compiled. That keeps the operands evaluated left to right.
static void binary(bool canAssign) {
  TokenType operatorType = parser.previous.type;
  ParseRule* rule = getRule(operatorType);
  bool flip = operatorType == TOKEN_GREATER ||
              operatorType == TOKEN_LESS_EQUAL;
  if (flip) emitNegate();
  parsePrecedence((Precedence)(rule->precedence + 1));
  if (flip) emitNegate();

  switch (operatorType) {
    case TOKEN_BANG_EQUAL:    emitBytes(OP_EQUAL, OP_NOT); break;
    case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
    case TOKEN_GREATER:       emitByte(OP_LESS); break;          // -a < -b
    case TOKEN_GREATER_EQUAL: emitBytes(OP_LESS, OP_NOT); break; // !(a < b)
    case TOKEN_LESS:          emitByte(OP_LESS); break;
    case TOKEN_LESS_EQUAL:    emitBytes(OP_LESS, OP_NOT); break; // !(-a < -b)
    case TOKEN_PLUS:          emitByte(OP_ADD); break;
    case TOKEN_MINUS:         emitNegate(); emitByte(OP_ADD); break; // a + -b
    case TOKEN_STAR:          emitByte(OP_MULTIPLY); break;
    case TOKEN_SLASH:         emitByte(OP_DIVIDE); break;
    default: return; // Unreachable.
  }
}
#else
static void binary(bool canAssign) {
  TokenType operatorType = parser.previous.type;
  ParseRule* rule = getRule(operatorType);
  parsePrecedence((Precedence)(rule->precedence + 1));

  switch (operatorType) {
    case TOKEN_BANG_EQUAL:    emitByte(OP_NOT_EQUAL); break;
    case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
    case TOKEN_GREATER:       emitByte(OP_GREATER); break;
    case TOKEN_GREATER_EQUAL: emitByte(OP_GREATER_EQUAL); break;
    case TOKEN_LESS:          emitByte(OP_LESS); break;
    case TOKEN_LESS_EQUAL:    emitByte(OP_LESS_EQUAL); break;
    case TOKEN_PLUS:          emitByte(OP_ADD); break;
    case TOKEN_MINUS:         emitByte(OP_SUBTRACT); break;
    case TOKEN_STAR:          emitByte(OP_MULTIPLY); break;
    case TOKEN_SLASH:         emitByte(OP_DIVIDE); break;
    default: return; // Unreachable.
  }
}
#endif
// 17.3: cond ? a : b. The condition is already compiled. Like C, the middle
// operand can be any expression, since "?" and ":" surround it, and the
// last one is parsed at PREC_CONDITIONAL so the operator is right
// associative: a ? b : c ? d : e means a ? b : (c ? d : e). Only one of the
// two branches runs.
static void conditional(bool canAssign) {
  int elseJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  expression();
  consume(TOKEN_COLON,
          "Expect ':' after then branch of conditional operator.");

  int endJump = emitJump(OP_JUMP);
  patchJump(elseJump);
  emitByte(OP_POP);
  parsePrecedence(PREC_CONDITIONAL);
  patchJump(endJump);
}
static void and_(bool canAssign) {
  int endJump = emitJump(OP_JUMP_IF_FALSE);

  emitByte(OP_POP);
  parsePrecedence(PREC_AND);

  patchJump(endJump);
}
static void or_(bool canAssign) {
  int elseJump = emitJump(OP_JUMP_IF_FALSE);
  int endJump = emitJump(OP_JUMP);

  patchJump(elseJump);
  emitByte(OP_POP);

  parsePrecedence(PREC_OR);
  patchJump(endJump);
}
static void literal(bool canAssign) {
  switch (parser.previous.type) {
#ifdef MINIMAL_INSTRUCTIONS
    case TOKEN_FALSE: emitSharedConstant(BOOL_VAL(false)); break;
    case TOKEN_NIL: emitSharedConstant(NIL_VAL); break;
    case TOKEN_TRUE: emitSharedConstant(BOOL_VAL(true)); break;
#else
    case TOKEN_FALSE: emitByte(OP_FALSE); break;
    case TOKEN_NIL: emitByte(OP_NIL); break;
    case TOKEN_TRUE: emitByte(OP_TRUE); break;
#endif
    default: return; // Unreachable.
  }
}
static void grouping(bool canAssign) {
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}
static void number(bool canAssign) {
  double value = strtod(parser.previous.start, NULL);
  emitConstant(NUMBER_VAL(value));
}
// The scanner's lexeme for a string is already just its contents (16.1).
static void string(bool canAssign) {
  emitConstant(OBJ_VAL(literalString(parser.previous.start,
                                     parser.previous.length)));
}
// Returns what the global instructions use as an operand for this name.
#ifdef GLOBAL_TABLE
static int identifierConstant(Token* name) {
  Value string = OBJ_VAL(literalString(name->start, name->length));
#ifndef NO_CONSTANT_DEDUP
  // 21.1: reuse the slot if this name is already in the constant table.
  Value index;
  if (tableGet(&stringConstants, string, &index)) {
    return (int)AS_NUMBER(index);
  }
#endif

  uint8_t constant = makeConstant(string);
#ifndef NO_CONSTANT_DEDUP
  tableSet(&stringConstants, string, NUMBER_VAL((double)constant));
#endif
  return constant;
}
#else
// 21.2: finds the slot for this name, or gives it a new, undefined one. The
// slots are in the VM so they last from one line of the REPL to the next.
static int identifierConstant(Token* name) {
  Value string = OBJ_VAL(literalString(name->start, name->length));
  Value slot;
  if (tableGet(&vm.globalNames, string, &slot)) {
    return (int)AS_NUMBER(slot);
  }

  if (vm.globalValues.count > UINT16_MAX) {
    error("Too many global variables.");
    return 0;
  }

  int index = vm.globalValues.count;
  writeValueArray(&vm.globalValues, EMPTY_VAL);
  writeValueArray(&vm.globalIdentifiers, string);
  tableSet(&vm.globalNames, string, NUMBER_VAL((double)index));
  return index;
}
#endif
static void emitGlobal(uint8_t instruction, int operand) {
  emitByte(instruction);
#ifndef GLOBAL_TABLE
  emitByte((operand >> 8) & 0xff);
#endif
  emitByte(operand & 0xff);
}
// 22.4: the first 256 slots use the book's one byte operand. After that the
// _LONG instruction has a two byte operand.
static void emitLocal(uint8_t instruction, uint8_t longInstruction,
                      int slot) {
  if (slot <= UINT8_MAX) {
    emitBytes(instruction, (uint8_t)slot);
  } else {
    emitByte(longInstruction);
    emitByte((slot >> 8) & 0xff);
    emitByte(slot & 0xff);
  }
}
#ifdef LOCAL_HASH
// 22.1: one hash table lookup instead of a scan through every local.
static int resolveLocal(Compiler* compiler, Token* name) {
  Value index;
  if (!tableGet(&compiler->localNames, localKey(name), &index)) return -1;

  int slot = (int)AS_NUMBER(index);
  if (compiler->locals[slot].depth == -1) {
    error("Can't read local variable in its own initializer.");
  }
  return slot;
}
#else
static bool identifiersEqual(Token* a, Token* b) {
  if (a->length != b->length) return false;
  return memcmp(a->start, b->start, a->length) == 0;
}
static int resolveLocal(Compiler* compiler, Token* name) {
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    Local* local = &compiler->locals[i];
    if (identifiersEqual(name, &local->name)) {
      if (local->depth == -1) {
        error("Can't read local variable in its own initializer.");
      }
      return i;
    }
  }

  return -1;
}
#endif
static void addLocal(Token name, bool isConst) {
  if (current->localCount == MAX_LOCALS) {
    error("Too many local variables in function.");
    return;
  }

  if (current->localCount == current->localCapacity) {
    int oldCapacity = current->localCapacity;
    current->localCapacity = GROW_CAPACITY(oldCapacity);
    current->locals = GROW_ARRAY(Local, current->locals, oldCapacity,
                                 current->localCapacity);
  }

  Local* local = &current->locals[current->localCount];
  local->name = name;
  local->depth = -1;
  local->isConst = isConst;
#ifdef LOCAL_HASH
  local->shadowed = -1;
  if (name.length > 0) {
    Value key = localKey(&name);
    Value previous;
    if (tableGet(&current->localNames, key, &previous)) {
      local->shadowed = (int)AS_NUMBER(previous);
    }
    tableSet(&current->localNames, key,
             NUMBER_VAL((double)current->localCount));
  }
#endif
  current->localCount++;
}
static void declareVariable(bool isConst) {
  if (current->scopeDepth == 0) return;

  Token* name = &parser.previous;
#ifdef LOCAL_HASH
  Value index;
  if (tableGet(&current->localNames, localKey(name), &index)) {
    int depth = current->locals[(int)AS_NUMBER(index)].depth;
    if (depth == -1 || depth >= current->scopeDepth) {
      error("Already a variable with this name in this scope.");
    }
  }
#else
  for (int i = current->localCount - 1; i >= 0; i--) {
    Local* local = &current->locals[i];
    if (local->depth != -1 && local->depth < current->scopeDepth) {
      break;
    }

    if (identifiersEqual(name, &local->name)) {
      error("Already a variable with this name in this scope.");
    }
  }
#endif

  addLocal(*name, isConst);
}
// 22.3: true if name is a global declared with const, either earlier in this
// code or by code that already ran.
static bool isConstGlobal(Token* name) {
  Value key = OBJ_VAL(literalString(name->start, name->length));
  Value unused;
  return tableGet(&newConstGlobals, key, &unused) ||
         tableGet(&vm.constGlobals, key, &unused);
}
static void assignError(Token* name) {
  char message[128];
  snprintf(message, sizeof(message), "Can't assign to constant '%.*s'.",
           name->length > 80 ? 80 : name->length, name->start);
  errorAt(name, message);
}
static void namedVariable(Token name, bool canAssign) {
  int arg = resolveLocal(current, &name);

  if (arg != -1) {
    if (canAssign && match(TOKEN_EQUAL)) {
      if (current->locals[arg].isConst) assignError(&name);
      expression();
      emitLocal(OP_SET_LOCAL, OP_SET_LOCAL_LONG, arg);
    } else {
      emitLocal(OP_GET_LOCAL, OP_GET_LOCAL_LONG, arg);
    }
    return;
  }

  arg = identifierConstant(&name);
  if (canAssign && match(TOKEN_EQUAL)) {
    if (isConstGlobal(&name)) assignError(&name);
    expression();
    emitGlobal(OP_SET_GLOBAL, arg);
  } else {
    emitGlobal(OP_GET_GLOBAL, arg);
  }
}
static void variable(bool canAssign) {
  namedVariable(parser.previous, canAssign);
}
static void unary(bool canAssign) {
  TokenType operatorType = parser.previous.type;

  // Compile the operand.
  parsePrecedence(PREC_UNARY);

  // Emit the operator instruction.
  switch (operatorType) {
    case TOKEN_BANG: emitByte(OP_NOT); break;
#ifdef MINIMAL_INSTRUCTIONS
    case TOKEN_MINUS: emitNegate(); break;
#else
    case TOKEN_MINUS: emitByte(OP_NEGATE); break;
#endif
    default: return; // Unreachable.
  }
}
ParseRule rules[] = {
  [TOKEN_LEFT_PAREN]    = {grouping, NULL,   PREC_NONE},
  [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_LEFT_BRACE]    = {NULL,     NULL,   PREC_NONE}, // [big]
  [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_DOT]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
  [TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
  [TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR},
  [TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR},
  [TOKEN_QUESTION]      = {NULL,     conditional, PREC_CONDITIONAL},
  [TOKEN_COLON]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_BANG]          = {unary,    NULL,   PREC_NONE},
  [TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
  [TOKEN_EQUAL]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
  [TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_LESS]          = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
  [TOKEN_STRING]        = {string,   NULL,   PREC_NONE},
  [TOKEN_NUMBER]        = {number,   NULL,   PREC_NONE},
  [TOKEN_INTERPOLATION] = {NULL,     NULL,   PREC_NONE},
  [TOKEN_AND]           = {NULL,     and_,   PREC_AND},
  [TOKEN_CASE]          = {NULL,     NULL,   PREC_NONE},
  [TOKEN_CLASS]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_CONST]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_CONTINUE]      = {NULL,     NULL,   PREC_NONE},
  [TOKEN_DEFAULT]       = {NULL,     NULL,   PREC_NONE},
  [TOKEN_DO]            = {NULL,     NULL,   PREC_NONE},
  [TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
  [TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FUN]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
  [TOKEN_NIL]           = {literal,  NULL,   PREC_NONE},
  [TOKEN_OR]            = {NULL,     or_,    PREC_OR},
  [TOKEN_PRINT]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SUPER]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SWITCH]        = {NULL,     NULL,   PREC_NONE},
  [TOKEN_THIS]          = {NULL,     NULL,   PREC_NONE},
  [TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE},
  [TOKEN_VAR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE},
};
#ifdef DEBUG_TRACE_PARSE
static const char* parseFnName(ParseFn fn) {
  if (fn == and_) return "and_";
  if (fn == binary) return "binary";
  if (fn == conditional) return "conditional";
  if (fn == grouping) return "grouping";
  if (fn == literal) return "literal";
  if (fn == number) return "number";
  if (fn == or_) return "or_";
  if (fn == string) return "string";
  if (fn == unary) return "unary";
  if (fn == variable) return "variable";
  return "?";
}
#endif
static void parsePrecedence(Precedence precedence) {
  TRACE_CALL("parsePrecedence(%s)", precedenceNames[precedence]);
  advance();
  ParseFn prefixRule = getRule(parser.previous.type)->prefix;
  if (prefixRule == NULL) {
    error("Expect expression.");
    TRACE_RETURN();
    return;
  }

  TRACE_CALL("%s() '%.*s'", parseFnName(prefixRule),
             parser.previous.length, parser.previous.start);
  bool canAssign = precedence <= PREC_ASSIGNMENT;
  prefixRule(canAssign);
  TRACE_RETURN();

  while (precedence <= getRule(parser.current.type)->precedence) {
    advance();
    ParseFn infixRule = getRule(parser.previous.type)->infix;
    TRACE_CALL("%s() '%.*s'", parseFnName(infixRule),
               parser.previous.length, parser.previous.start);
    infixRule(canAssign);
    TRACE_RETURN();
  }

  if (canAssign && match(TOKEN_EQUAL)) {
    error("Invalid assignment target.");
  }
  TRACE_RETURN();
}
static int parseVariable(const char* errorMessage, bool isConst) {
  consume(TOKEN_IDENTIFIER, errorMessage);

  declareVariable(isConst);
  if (current->scopeDepth > 0) return 0;

  return identifierConstant(&parser.previous);
}
static void markInitialized() {
  current->locals[current->localCount - 1].depth = current->scopeDepth;
}
static void defineVariable(int global) {
  if (current->scopeDepth > 0) {
    markInitialized();
    return;
  }

  emitGlobal(OP_DEFINE_GLOBAL, global);
}
static ParseRule* getRule(TokenType type) {
  return &rules[type];
}
static void expression() {
  TRACE_CALL("expression()");
  parsePrecedence(PREC_ASSIGNMENT);
  TRACE_RETURN();
}
static void block() {
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    declaration();
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}
// 22.3: const works like var, but needs an initializer and can't be
// assigned. A const global also can't be declared again.
static void varDeclaration(bool isConst) {
  int global = parseVariable(isConst ? "Expect constant name."
                                     : "Expect variable name.", isConst);
  Token name = parser.previous;
  if (current->scopeDepth == 0 && isConstGlobal(&name)) {
    errorAt(&name, "Can't redeclare a constant.");
  }

  if (match(TOKEN_EQUAL)) {
    expression();
  } else {
    if (isConst) errorAtCurrent("Expect '=' after constant name.");
    emitNil();
  }
  consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

  defineVariable(global);
  if (isConst && current->scopeDepth == 0) {
    tableSet(&newConstGlobals,
             OBJ_VAL(literalString(name.start, name.length)),
             BOOL_VAL(true));
  }
}
static void expressionStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(OP_POP);
}
static void beginLoop(Loop* loop, int start) {
  loop->enclosing = current->loop;
  loop->start = start;
  loop->scopeDepth = current->scopeDepth;
  loop->continueJumps = NULL;
  loop->continueCount = 0;
  loop->continueCapacity = 0;
  current->loop = loop;
}
// 23.3: points the forward continue jumps at the current spot.
static void patchContinues(Loop* loop) {
  for (int i = 0; i < loop->continueCount; i++) {
    patchJump(loop->continueJumps[i]);
  }
  loop->continueCount = 0;
}
static void endLoop(Loop* loop) {
  FREE_ARRAY(int, loop->continueJumps, loop->continueCapacity);
  current->loop = loop->enclosing;
}
static void forStatement() {
  beginScope();
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
  if (match(TOKEN_SEMICOLON)) {
    // No initializer.
  } else if (match(TOKEN_VAR)) {
    varDeclaration(false);
  } else {
    expressionStatement();
  }

  int loopStart = currentChunk()->count;
  Loop loop;
  beginLoop(&loop, loopStart);

  int exitJump = -1;
  if (!match(TOKEN_SEMICOLON)) {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

    // Jump out of the loop if the condition is false.
    exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // Condition.
  }

  if (!match(TOKEN_RIGHT_PAREN)) {
    int bodyJump = emitJump(OP_JUMP);
    int incrementStart = currentChunk()->count;
    expression();
    emitByte(OP_POP);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

    emitLoop(loopStart);
    loopStart = incrementStart;
    // 23.2: continue runs the increment next.
    loop.start = incrementStart;
    patchJump(bodyJump);
  }

  statement();
  emitLoop(loopStart);

  if (exitJump != -1) {
    patchJump(exitJump);
    emitByte(OP_POP); // Condition.
  }

  endLoop(&loop);
  endScope();
}
static void ifStatement() {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  int thenJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  statement();

  int elseJump = emitJump(OP_JUMP);

  patchJump(thenJump);
  emitByte(OP_POP);

  if (match(TOKEN_ELSE)) statement();
  patchJump(elseJump);
}
static void printStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after value.");
  emitByte(OP_PRINT);
}
static void whileStatement() {
  int loopStart = currentChunk()->count;
  Loop loop;
  beginLoop(&loop, loopStart);

  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  int exitJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  statement();
  emitLoop(loopStart);

  patchJump(exitJump);
  emitByte(OP_POP);
  endLoop(&loop);
}
// 23.3: do A while (cond) B
// Runs A, stops if cond is false, otherwise runs B and starts again. With a
// ";" instead of B, it is an ordinary do-while loop. A continue in A goes to
// the condition, and a continue in B goes back to A.
static void doStatement() {
  int loopStart = currentChunk()->count;
  Loop loop;
  beginLoop(&loop, -1);

  statement();
  patchContinues(&loop);

  consume(TOKEN_WHILE, "Expect 'while' after 'do' body.");
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  int exitJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  if (!match(TOKEN_SEMICOLON)) {
    loop.start = loopStart;
    statement();
  }
  emitLoop(loopStart);

  patchJump(exitJump);
  emitByte(OP_POP);
  endLoop(&loop);
}
// 23.2
static void continueStatement() {
  Loop* loop = current->loop;
  if (loop == NULL) {
    error("Can't use 'continue' outside of a loop.");
  }
  consume(TOKEN_SEMICOLON, "Expect ';' after 'continue'.");
  if (loop == NULL) return;

  // Pop the locals declared inside the loop, but keep them in the compiler,
  // since the code after the continue can still use them.
  for (int i = current->localCount - 1;
       i >= 0 && current->locals[i].depth > loop->scopeDepth; i--) {
    emitByte(OP_POP);
  }

  if (loop->start != -1) {
    emitLoop(loop->start);
    return;
  }

  if (loop->continueCount == loop->continueCapacity) {
    int oldCapacity = loop->continueCapacity;
    loop->continueCapacity = GROW_CAPACITY(oldCapacity);
    loop->continueJumps = GROW_ARRAY(int, loop->continueJumps, oldCapacity,
                                     loop->continueCapacity);
  }
  loop->continueJumps[loop->continueCount++] = emitJump(OP_JUMP);
}
// 23.1. The switch value stays on the stack while the cases run, so it is
// added as a local with no name. Then the slots of any locals in the cases
// are right, and continue pops it. Each case is its own scope, so it can
// declare variables.
static void switchStatement() {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'switch'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after value.");
  consume(TOKEN_LEFT_BRACE, "Expect '{' before switch cases.");

  beginScope();
  Token hidden = parser.previous;
  hidden.length = 0;
  addLocal(hidden, false);
  if (current->localCount > 0) markInitialized();

  int state = 0; // 0: before all cases, 1: before default, 2: after default.
  bool caseOpen = false;
  int* caseEnds = NULL;
  int caseCount = 0;
  int caseCapacity = 0;
  int previousCaseSkip = -1;

  for (;;) {
    bool isCase = check(TOKEN_CASE);
    bool atEnd = check(TOKEN_RIGHT_BRACE) || check(TOKEN_EOF);
    if (!isCase && !check(TOKEN_DEFAULT) && !atEnd) {
      // A statement inside the current case.
      if (state == 0) {
        errorAtCurrent("Can't have statements before any case.");
      }
      declaration();
      continue;
    }

    // The end of a case: close its scope and jump to the end of the switch.
    if (caseOpen) {
      endScope();
      caseOpen = false;
    }
    if (state == 1) {
      if (caseCount == caseCapacity) {
        int oldCapacity = caseCapacity;
        caseCapacity = GROW_CAPACITY(oldCapacity);
        caseEnds = GROW_ARRAY(int, caseEnds, oldCapacity, caseCapacity);
      }
      caseEnds[caseCount++] = emitJump(OP_JUMP);

      // If the previous case didn't match, it jumps here.
      patchJump(previousCaseSkip);
      emitByte(OP_POP);
    }
    if (atEnd) break;

    advance();
    if (state == 2) {
      error("Can't have another case or default after the default case.");
    }
    if (isCase) {
      state = 1;
      emitByte(OP_DUP);
      expression();
      consume(TOKEN_COLON, "Expect ':' after case value.");
      emitByte(OP_EQUAL);
      previousCaseSkip = emitJump(OP_JUMP_IF_FALSE);
      emitByte(OP_POP);
    } else {
      state = 2;
      consume(TOKEN_COLON, "Expect ':' after default.");
    }
    beginScope();
    caseOpen = true;
  }

  for (int i = 0; i < caseCount; i++) {
    patchJump(caseEnds[i]);
  }
  FREE_ARRAY(int, caseEnds, caseCapacity);

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after switch cases.");
  endScope(); // Pops the switch value.
}
static void synchronize() {
  parser.panicMode = false;

  while (parser.current.type != TOKEN_EOF) {
    if (parser.previous.type == TOKEN_SEMICOLON) return;
    switch (parser.current.type) {
      case TOKEN_CLASS:
      case TOKEN_CONST:
      case TOKEN_FUN:
      case TOKEN_VAR:
      case TOKEN_FOR:
      case TOKEN_IF:
      case TOKEN_WHILE:
      case TOKEN_DO:
      case TOKEN_SWITCH:
      case TOKEN_PRINT:
      case TOKEN_RETURN:
        return;

      default:
        ; // Do nothing.
    }

    advance();
  }
}
static void declaration() {
  if (match(TOKEN_VAR)) {
    varDeclaration(false);
  } else if (match(TOKEN_CONST)) {
    varDeclaration(true);
  } else {
    statement();
  }

  if (parser.panicMode) synchronize();
}
static void statement() {
  if (match(TOKEN_PRINT)) {
    printStatement();
  } else if (match(TOKEN_FOR)) {
    forStatement();
  } else if (match(TOKEN_IF)) {
    ifStatement();
  } else if (match(TOKEN_WHILE)) {
    whileStatement();
  } else if (match(TOKEN_DO)) {
    doStatement();
  } else if (match(TOKEN_SWITCH)) {
    switchStatement();
  } else if (match(TOKEN_CONTINUE)) {
    continueStatement();
  } else if (match(TOKEN_LEFT_BRACE)) {
    beginScope();
    block();
    endScope();
  } else {
    expressionStatement();
  }
}

bool compile(const char* source, Chunk* chunk) {
  initScanner(source);
  Compiler compiler;
  initCompiler(&compiler);
  compilingChunk = chunk;

  parser.hadError = false;
  parser.panicMode = false;
#if defined(GLOBAL_TABLE) && !defined(NO_CONSTANT_DEDUP)
  initTable(&stringConstants);
#endif
  initTable(&newConstGlobals);

  advance();

  while (!match(TOKEN_EOF)) {
    declaration();
  }

  endCompiler();
#if defined(GLOBAL_TABLE) && !defined(NO_CONSTANT_DEDUP)
  freeTable(&stringConstants);
#endif
  if (!parser.hadError) tableAddAll(&newConstGlobals, &vm.constGlobals);
  freeTable(&newConstGlobals);
  return !parser.hadError;
}
