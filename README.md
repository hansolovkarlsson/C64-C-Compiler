# cc64 - a minimal C compiler for the Commodore 64

`cc64` compiles a small subset of C directly to 6502 assembly in the
exact syntax your `c64asm.c` assembler expects. This is step 1 of an
incremental plan: get a solid, verified minimal subset working first,
then add features (pointers, recursion, structs, ...) on top of a
foundation that's already known to generate correct code.

## Building

Portable C99, no dependencies:

```sh
cc -std=c99 -O2 -o cc64 cc64.c
cc -std=c99 -O2 -o c64asm c64asm.c   # your existing assembler
```

Both build cleanly with `clang` on Apple Silicon or `gcc`/`cc` on Linux.

## Using it

```sh
./cc64 program.c -o program.asm
./c64asm program.asm -o program.prg
```

or, to do both in one step:

```sh
./build.sh program.c program.prg
```

Load `program.prg` in VICE (or a real C64) the normal way; it's built
with a BASIC `10 SYS ...` loader stub, so `LOAD` then `RUN` works.

## What's supported (the "minimal" subset)

- **Types:** `int` (16-bit, signed), `char` (8-bit, **unsigned** - see
  below), `void`.
- **Declarations:** globals and locals, with an optional 1-D array
  form (`int a[10];`, `char buf[40];`), and an optional constant
  literal initializer for scalar globals (`int x = 5;`, `int y = -3;`).
  Array initializers aren't supported yet (arrays start zero-filled).
- **Functions:** typed parameters, typed return value, forward calls in
  any order (any function can call any other regardless of which is
  defined first in the file - see "Two-pass compilation" below). **No
  recursion**, direct or indirect - see "Why no recursion" below. The
  compiler detects and rejects recursive call cycles at compile time
  with a clear error, rather than letting them silently corrupt data.
- **Statements:** `if`/`else`, `while`, `for`, `break`, `continue`,
  `return`, blocks, local declarations (anywhere in a block), empty
  statement.
- **Operators:** `+ - * / % & | ^ ~ ! << >> && || == != < > <= >=`
  `= += -= *= /= %= &= |= ^= <<= >>=` and pre/post `++`/`--`.
  `/` and `%` are correctly signed (truncate toward zero; remainder
  takes the sign of the dividend, matching C99).
- **Literals:** decimal and `0x` hex integers, `'c'` char literals
  with `\n \t \\ \' \" \0` escapes, `"string"` literals (only as the
  sole argument to `puts()` - see below).
- **Builtins:** `putchar(x)`, `puts("literal")`, `peek(addr)`,
  `poke(addr, val)`.

## Not supported yet (planned for later steps)

Pointers (`*`, `&`), structs/unions, typedefs, function recursion,
passing arrays to functions, multi-dimensional arrays, floating
point, the preprocessor, `do`/`while`, `switch`, multiple source
files, and anything like `printf` (there's no variadic support or
string formatting - see `print_uint`/`print_int` in
`tests/features.c` for a hand-written decimal printer you can copy
into your own programs in the meantime).

## Design notes

### Why no recursion (yet)

Because there's no C-level recursion in this step, every function's
parameters and local variables get **fixed, static storage** (like
old-style non-reentrant compilers) instead of a real per-call stack
frame. This is a deliberate simplification for the first step: it
means no frame pointer, no stack-relative addressing, and much
simpler code generation - fewer places for step-1 bugs to hide. A
software *operand* stack is still used for evaluating nested
expressions (`a * (b + c)` etc.) since that's independent of function
recursion and will still be needed once real recursion is added.

If you write a recursive function, `cc64` will refuse to compile it
with a clear error rather than silently generating code that
corrupts the function's own parameters/locals on re-entry.

### Two-pass compilation

`cc64` scans all top-level declarations first (function signatures
and globals) before generating any code. That means you don't need
forward declarations for the common case - any function can call any
other function anywhere in the file. (You *can* still write a
prototype like `int foo(int x);` if you want one for documentation;
it's just not required.)

### `char` is unsigned

To keep the first step simple, `char` is treated as unsigned 8-bit
(no sign extension logic needed). This is a common choice for small
8-bit-target C compilers and matches how many people use `char` on
this platform anyway (as a byte type, not as `signed char`).

### PETSCII and case

The C64 boots into its default character set ("uppercase/graphics"),
where only the codes in the `$41`-`$5A` range render as letters (and
only as uppercase) - the `$C1`-`$DA` range your `c64asm.c`'s
`ascii_to_petscii()` uses for uppercase source characters is
**graphics symbols**, not lowercase letters, in that default mode.
Those codes only render as letters once the C64 has been switched
into its second character set, via PETSCII control code 14. `cc64`
emits that switch once, automatically, as the very first thing your
program does (before `main` even runs), so you don't have to think
about it. With that in place, `puts("...")` (converted at compile
time) and `putchar(x)` (converted at runtime, so it works even for
values that aren't compile-time constants) both display text in the
**same case you wrote it** - no flipping, no workaround needed. Plain
`char` variables/arrays are **not** auto-converted (a byte is just a
byte until you print it) - only the print path applies PETSCII
mapping.

(This one was actually caught after the fact: my first version didn't
emit the charset switch, so all-uppercase test output rendered as
graphics characters on real hardware. My verification emulator didn't
catch it either, because it was decoding `$C1`-`$DA` as lowercase
unconditionally rather than modeling the two real character sets and
the control code that switches between them - both the compiler and
the emulator are fixed now.)

### Zero-page usage

```
$02/$03  __zpSP   soft operand-stack pointer
$F3/$F4  __zpAP   effective-address pointer (array/peek/poke)
$F5/$F6  __zpAP2  saved-AP scratch (assign/compound-assign to arrays)
$F7/$F8  __zpT1   runtime-library scratch
$F9/$FA  __zpT0   runtime-library scratch
$FB/$FC  __zpR    primary register / function return value
$FD/$FE  __zpR2   secondary operand register
```

These are the classic "free for machine code" zero-page bytes used
when a program takes over via `SYS` and isn't concurrently relying on
BASIC floating point or RS-232 routines. All compiler-generated
assembly symbols (`__fn_`, `__g_`, `__L`, `__rt_`, `__zp`) live in
C's own reserved-identifier namespace (leading double underscore, or
underscore + capital), so they can never collide with a valid user
identifier.

### Calling convention

Arguments are copied into the callee's fixed parameter slots, then a
plain `JSR`. Return values are left in the primary register `__zpR`
at `RTS` time - every `return expr;` just computes `expr` into `__zpR`
and returns; there's no shared epilogue to tear down, since there's
no stack frame to unwind.

## Testing

There's no VICE/x64 or other 6502 emulator in this environment, so
verification here was done with a small purpose-built emulator,
`mini6502.py`, that implements exactly the opcode/addressing-mode
subset `cc64` emits and traps `$FFD2` (CHROUT) in software:

```sh
python3 mini6502.py program.prg program.lst
```

(`c64asm --listing program.lst` produces the listing it needs to find
the real code entry point, skipping the non-executable BASIC stub.)

`tests/features.c` is a comprehensive check covering signed
arithmetic and truncation rules, bitwise ops and shifts, all six
comparisons (including negative operands), `&&`/`||` short-circuit
evaluation (verified via a side-effect counter), non-recursive
function calls, array fill/read/compound-assign/inc-dec, `peek`/
`poke`, `break`/`continue`, and pre/post `++`/`--`. Every computed
value was checked against hand-calculated expected output.
`tests/forward.c` checks that forward/backward call references both
work regardless of declaration order. `tests/hello.c` is the basic
smoke test for the whole pipeline (BASIC stub, zero-page init, stack
init, `puts`/`putchar`, PETSCII case mapping).

Run all three:

```sh
for f in hello features forward; do
    ./cc64 tests/$f.c -o tests/$f.asm
    ./c64asm tests/$f.asm -o tests/$f.prg --listing tests/$f.lst
    python3 mini6502.py tests/$f.prg tests/$f.lst
done
```

## Roadmap (next steps, in a sensible order)

1. Pointers and `&`/`*` - unlocks passing arrays to functions,
   `char*` strings as real values (not just literals), and is a
   prerequisite for...
2. Function recursion - needs a real per-call stack frame for
   parameters/locals instead of static storage, built on top of the
   pointer support above.
3. `do`/`while`, `switch`.
4. `struct`s.
5. A tiny standard library: real `printf`-lite, string helpers.
6. Multiple source files / a simple `#include`.
