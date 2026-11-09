// Chapter 23: the book's control flow, switch (23.1), continue (23.2) and
// the do-while loop with a middle (23.3).
#include <stdio.h>

#include "../src/vm.h"

// Runs a program, then prints how many values it left on the stack. Every
// program should leave 0.
static void run(const char* source) {
  interpret(source);
  printf("stack: %d\n", (int)(vm.stackTop - vm.stack));
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  initVM();

  printf("-- the book's control flow\n");
  run("if (true) print \"then\"; else print \"else\";");
  run("if (nil) print \"then\"; else print \"else\";");
  run("if (false) print \"skipped\"; print \"after\";");
  run("print false or \"right\"; print 1 and 2; print nil and 1;");
  run("print 1 or 2; print false and 1;");
  run("var i = 0; while (i < 3) { print i; i = i + 1; }");
  run("for (var i = 0; i < 3; i = i + 1) print i;");
  run("{ var n = 0; for (; n < 2;) { print n; n = n + 1; } }");
  run("for (var a = 1; a < 100; a = a * 10) { var b = a; print b; }");

  printf("-- 23.1: switch\n");
  const char* sw =
      "for (var i = 0; i < 5; i = i + 1) {"
      "  switch (i) {"
      "    case 1: print \"one\";"
      "    case 2: print \"two\"; print \"still two\";"
      "    case 1 + 2: print \"three\";"
      "    default: print \"default\";"
      "  }"
      "}";
  run(sw);
  run("switch (\"b\") { case \"a\": print \"a\"; case \"b\": print \"b\"; }");
  run("switch (nil) { case false: print \"false\"; case nil: print \"nil\"; }");
  run("switch (9) { case 1: print \"one\"; }");
  run("switch (9) { default: print \"only default\"; }");
  run("switch (9) { }");
  // The last case matches and there is no default.
  run("{ var a = 1; switch (3) { case 3: print \"three\"; } var b = 2;"
      "  print a; print b; }");
  // A local inside a case.
  run("{ var a = \"a\"; switch (1) { case 1: { var b = \"b\"; print b; } }"
      "  print a; }");
  run("switch (1) { case 1: var x = \"case one\"; print x;"
      "  case 2: var x = \"case two\"; print x; }");
  run("switch (2) { case 1: var x = \"case one\"; print x;"
      "  case 2: var x = \"case two\"; print x; }");
  run("switch (1) { print \"too early\"; case 1: print 1; }");
  run("switch (1) { default: print 1; case 1: print 1; }");
  run("switch (1) { case 1 print 1; }");

  printf("-- 23.2: continue\n");
  run("for (var i = 0; i < 5; i = i + 1) {"
      "  if (i == 1 or i == 3) continue;"
      "  print i;"
      "}");
  run("{ var i = 0;"
      "  while (i < 5) {"
      "    i = i + 1;"
      "    var local = i * 10;"
      "    { var nested = local; if (i == 2) continue; }"
      "    if (i == 4) continue;"
      "    print local;"
      "  }"
      "  print \"done\"; print i; }");
  run("{ var kept = \"kept\";"
      "  for (var i = 0; i < 3; i = i + 1) {"
      "    var a = i; var b = a;"
      "    if (b == 1) continue;"
      "    print b;"
      "  }"
      "  print kept; }");
  // continue inside a while inside a for goes to the while.
  run("for (var i = 0; i < 2; i = i + 1) {"
      "  var j = 0;"
      "  while (j < 3) { j = j + 1; if (j == 2) continue; print i * 10 + j; }"
      "}");
  // continue inside a switch inside a loop.
  run("{ var kept = \"kept\";"
      "  for (var i = 0; i < 4; i = i + 1) {"
      "    switch (i) { case 1: continue; case 2: { var x = 1; continue; } }"
      "    print i;"
      "  }"
      "  print kept; }");
  run("for (var i = 0; i < 2;) { i = i + 1; if (i == 1) continue; print i; }");
  run("continue;");
  run("{ continue; }");
  run("if (true) continue;");

  printf("-- 23.3: do-while with a middle\n");
  run("do print \"runs once\"; while (false);");
  run("{ var i = 0; do { print i; i = i + 1; } while (i < 3); }");
  // Joining items with a separator: the item part runs one more time than
  // the separator part.
  run("{ var i = 1; var result = \"\";"
      "  do {"
      "    result = result + \"item\";"
      "    i = i + 1;"
      "  } while (i <= 3) {"
      "    result = result + \", \";"
      "  }"
      "  print result; }");
  // continue in the first part goes to the condition, so this stops.
  run("{ var i = 0; do { i = i + 1; continue; print \"never\"; }"
      "  while (i < 3); print i; }");
  // continue in the second part goes back to the first part.
  run("{ var i = 0;"
      "  do { i = i + 1; print i; } while (i < 4) {"
      "    var skip = i == 2;"
      "    if (skip) continue;"
      "    print \"between\";"
      "  } }");
  run("do print 1; (true);");

  freeVM();
  return 0;
}
