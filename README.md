# Crafting Interpreters — Chapters 22 & 23 Challenges

Solutions to the end-of-chapter challenges from [_Crafting Interpreters_](https://craftinginterpreters.com/) by Robert Nystrom, chapters **22 (Local Variables)** and **23 (Jumping Back and Forth)**. This is clox, the bytecode virtual machine in C, with the complete code through chapter 23, and it builds on my chapters 19-21 solutions. It runs programs made of `print`, expression and `var` statements, blocks, `if`, `while`, `for`, `and` and `or`, plus the new statements below.

## Build and run

```sh
make                  # builds build/clox (a REPL, or pass a file)
make hashlocal        # build/clox-hashlocal, finds locals with a hash table (22.1)
make constant         # build/clox-constant, strings that point at the source (19.2)
make table            # build/clox-table, the book's table of globals (21.1)
make minimal          # build/clox-minimal, the reduced instruction set (18.1)
make trace            # prints the bytecode and each instruction as it runs
./run_tests.sh        # runs the tests in all five builds
make bench            # the benchmarks for 20.3, 21.1, 21.2 and 22.1
```

## Chapter 22 — Local Variables

1. **Faster lookups.** With `-DLOCAL_HASH`, the compiler keeps a hash table
   from each name to its innermost local, and each local remembers the one it
   hides so `endScope()` can put it back. `bench/locals_bench.c` compares it
   with the book's linear scan. Whether it's worth it is a written answer.
2. **`var a = a;`** Written answer (submitted separately).
3. **`const`.** `const x = 1;` declares a variable that can't be assigned,
   local or global. Assigning to one, leaving out the initializer, or
   declaring a global constant again is a compile error. Global constants are
   remembered in `vm.constGlobals`, so later lines of the REPL can't assign to
   them either.
4. **More than 256 locals.** `OP_GET_LOCAL_LONG` and `OP_SET_LOCAL_LONG` take a
   two byte slot, so 65,536 locals can be in scope. The compiler's locals and
   the VM's stack are growable arrays, like my 15.3 stack.

## Chapter 23 — Jumping Back and Forth

1. **`switch`.** No fallthrough, cases compared with `==`. The switch value is
   kept in a local with no name, so locals inside cases get the right slots,
   and each case is its own scope.
2. **`continue`.** Works in `while`, `for` and `do` loops, pops the locals
   declared inside the loop, and goes to the increment in a `for` loop.
3. **A new loop.** `do A while (cond) B` runs `A`, stops if `cond` is false,
   and otherwise runs `B` and starts again. With `;` in place of `B` it is a
   plain do-while loop.

## Files changed from my chapter 21 code

| File | Change |
|---|---|
| `compiler.c` | Locals, scopes, `if`, `while`, `for`, `and`, `or` from the book; hash table lookups (22.1); `const` (22.3); long slots (22.4); `switch`, `continue` and `do` (23.1-23.3) |
| `scanner.h`, `scanner.c` | `const`, `switch`, `case`, `default`, `continue` and `do` keywords |
| `chunk.h`, `debug.c` | `OP_GET_LOCAL`, `OP_SET_LOCAL`, `OP_LOOP`; the `_LONG` local instructions (22.4); `OP_DUP` (23.1) |
| `vm.h`, `vm.c` | The new instructions; a growable stack (22.4); `constGlobals` (22.3) |
| `common.h` | Lists the `LOCAL_HASH` option |

## Tests

`./run_tests.sh` builds every program in `tests/`, runs it, and compares its
output with its `.expected` file, in five builds: default, minimal, constant,
table and hashlocal. A test uses `<name>.<build>.expected` if it has one. All
55 checks pass, and they also pass with
`SANITIZE="-fsanitize=address,undefined -g"`. The new tests are
`tests/ch22_locals.c` and `tests/ch23_control.c`, and each program in them
also checks that it leaves nothing on the stack.
