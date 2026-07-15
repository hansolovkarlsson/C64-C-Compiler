# cc64 - a minimal C compiler for the Commodore 64

`cc64` compiles a small subset of C directly to 6502 assembly in the
exact syntax your `c64asm.c` assembler expects. This is built
incrementally: get a solid, verified minimal subset working first (a
step-1 "int/char, no pointers" core), then add features on top of a
foundation that's already known to generate correct code. Pointers,
full function recursion, `#include`, and a small standard library are
all in now; `struct`s are the likely next step.

The source is split into one file per compiler phase under `src/`,
each with a substantial comment explaining what that phase does and
why - see "Source layout" below if you're reading this to learn how a
small compiler like this is put together, not just to use it.

## Building

Portable C99, no dependencies. A Makefile builds `cc64` from `src/*.c`:

```sh
make
cc -std=c99 -O2 -o c64asm c64asm.c   # your existing assembler
```

(`make clean` removes the built binary.) Both build cleanly with
`clang` on Apple Silicon or `gcc`/`cc` on Linux. If you'd rather build
by hand without the Makefile: `cc -std=c99 -O2 -o cc64 src/*.c`.

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

### Using the standard library

`#include <string.h>` and `#include <print.h>` (in `lib/`) are found
automatically with no setup, as long as `cc64` is invoked with a path
that has a `/` in it - `./cc64`, `../build/cc64`, `/opt/cc64/cc64`,
all fine - which covers every example in this README and `build.sh`.
(If `cc64` is on your `PATH` and invoked by bare name, there's no
portable, dependency-free way to recover its install location from
C99 alone, so that one case needs an explicit `-I`:)

```sh
./cc64 program.c -o program.asm -I /path/to/cc64/lib
```

`#include "local.h"` (quoted) looks next to the file doing the
including first, then falls back to the same search path as `<...>`
- see "The standard library" below for what's in `lib/`, and
`src/cc64.h`'s "HOW #include WORKS" note for the mechanism itself.

## Source layout

Each file below corresponds to one phase you'd recognize from a
compilers course; `src/cc64.h`'s own header comment gives the same
map with more detail on how the phases fit together, plus notes on
the compiler's overall architecture (why everything flows through a
"register", why there's no call stack, why parsing happens in two
passes). Reading the files in roughly this order is a reasonable way
to approach the codebase for the first time:

| File | What it does |
|---|---|
| `src/cc64.h` | Shared types and cross-module declarations; start here for the architecture overview |
| `src/lexer.c` | Source text -> tokens, and `#include` splicing |
| `src/ast.c` | The AST node constructor |
| `src/symtab.c` | Symbol tables, plus the minimal type inference used for pointer arithmetic |
| `src/parser.c` | Recursive-descent parsing (tokens -> AST), and the two-pass driver (`pass_a`/`pass_b`) |
| `src/codegen.c` | Shared codegen utilities: emitting a line of assembly, label generation |
| `src/codegen_runtime.c` | The fixed 6502 runtime library (multiply, divide, comparisons, string printing) |
| `src/codegen_expr.c` | Expression codegen - the largest file, where most operators and pointer handling live |
| `src/codegen_stmt.c` | Statement codegen, storage layout, and the per-function frame save/restore routines that make recursion work |
| `src/main.c` | The command-line driver tying every phase together |
| `lib/string.h`, `lib/print.h` | The standard library - see below |


## What's supported

- **Types:** `int` (16-bit, signed), `char` (8-bit, **unsigned** - see
  below), `void`, and single-level pointers to either (`int *`, `char *`).
- **Declarations:** globals and locals, with an optional 1-D array
  form (`int a[10];`, `char buf[40];`), and an optional constant
  literal initializer for scalar (non-pointer) globals (`int x = 5;`,
  `int y = -3;`). Array initializers and pointer initializers on
  *globals* aren't supported yet (arrays start zero-filled; give a
  pointer its value inside a function instead). Local pointer
  declarations *can* have an initializer, including a string literal
  (`char *s = "hi";`).
- **Functions:** typed parameters (including pointer parameters),
  typed return value (including pointer return types), forward calls
  in any order (any function can call any other regardless of which
  is defined first in the file - see "Two-pass compilation" below),
  and **full recursion**, direct or mutual - see "How recursion
  works" below for both the mechanism and its limits (a 256-byte
  per-call frame cap, and runtime overflow guards that halt with a
  clear on-screen error instead of silently corrupting memory when
  recursion runs away).
- **Pointers:** `&x` (address-of - works on scalars and array
  elements; safe on locals too, since there's no call stack for them
  to dangle from), `*p` (dereference, usable as both an rvalue and an
  assignment target, including `*p += n`, `(*p)++`, etc.), `p[i]`
  indexing through a pointer (not just through a true array),
  pointer arithmetic (`p + n`, `p - n`, `p++`/`p--`, all correctly
  scaled by `sizeof(*p)` - 2 bytes for `int*`, 1 for `char*`),
  pointer-minus-pointer (`p2 - p1`, giving an element count, not a
  byte count), and pointer comparisons (`<` `>` `<=` `>=` `==` `!=`,
  using an **unsigned** comparison since addresses aren't signed
  quantities - unlike plain `int` comparisons, which are signed).
  Arrays decay to a pointer to their first element wherever a pointer
  is expected (passing an array as a function argument, assigning an
  array to a pointer variable, etc.).
- **Statements:** `if`/`else`, `while`, `for`, `break`, `continue`,
  `return`, blocks, local declarations (anywhere in a block), empty
  statement.
- **Operators:** `+ - * / % & | ^ ~ ! << >> && || == != < > <= >=`
  `= += -= *= /= %= &= |= ^= <<= >>=` and pre/post `++`/`--`.
  `/` and `%` are correctly signed (truncate toward zero; remainder
  takes the sign of the dividend, matching C99).
- **Literals:** decimal and `0x` hex integers, `'c'` char literals
  with `\n \t \\ \' \" \0` escapes, and `"string"` literals, which are
  real `char*` values now (interned as data, not just accepted by
  `puts()` - see below).
- **Builtins:** `putchar(x)`, `puts(s)` (accepts any `char*` - a
  literal, a variable, a buffer you built at runtime, all walked at
  runtime up to the first zero byte), `peek(addr)`, `poke(addr, val)`.
- **Light type checking:** the compiler doesn't do full C type
  checking, but it does catch some common pointer mistakes at compile
  time rather than letting them corrupt memory silently: dereferencing
  a non-pointer, `pointer + pointer`, `int - pointer`, passing a
  plain value where a function expects a pointer (or vice versa), and
  returning a plain value from a function declared to return a
  pointer (or vice versa). `0` is always accepted as a valid pointer
  value ("null") in these checks.
- **`#include`**, both `"quoted"` (searched next to the including
  file, then falling back to the same path as angle brackets) and
  `<angle-bracket>` (searched via `-I` directories and an
  automatically-detected `lib/` next to the compiler itself - see
  "Using the standard library" above). Every file is implicitly
  include-once by resolved path, so headers don't need manual include
  guards. This is the *only* preprocessor functionality - no `#define`,
  no `#ifdef`, nothing else starting with `#` - see `src/cc64.h`'s
  "HOW #include WORKS" note for the (simple) mechanism.
- **A small standard library** (`lib/string.h`, `lib/print.h`) - see
  "The standard library" below for the full list.

## Not supported yet (planned for later steps)

Pointer-to-pointer, function pointers, arrays of pointers, array
*parameters* written with `[]` syntax (use `type *name` instead - it
receives exactly the same decayed pointer), `struct`/`union`,
`typedef`s, multi-dimensional arrays, floating point, `do`/`while`,
`switch`, and anything like `printf` (there's no variadic-function
support, so no way to accept a runtime-variable number of arguments -
`print_int`/`print_hex` in `lib/print.h` cover the common cases with
fixed-arity calls instead). The preprocessor is limited to
`#include` - no `#define`, no macros, no conditional compilation.

## Design notes

### How recursion works

Every function's parameters and locals still get **fixed, static
storage** (like old-style non-reentrant compilers) - reading or
writing a variable is a plain absolute load/store, with no frame
pointer or stack-relative addressing anywhere in expression codegen.
Recursion is layered *on top of* that model rather than replacing it:
for every function, the compiler emits a `pushframe` routine (copy
that function's entire parameter+local block onto a software call
stack) and a `popframe` routine (copy it back), and wraps every call
site with them - save the callee's current frame, write the new
arguments, `JSR`, then restore. When a recursive chain unwinds, each
level finds its variables exactly as it left them, even though every
level used the same physical addresses. The long comment above
`emit_function()` in `src/codegen_stmt.c` walks through a factorial
call step by step if you want to see the mechanism in motion.

Two practical limits fall out of the design, both enforced rather
than left as silent traps:

- **A function's frame (parameters + locals, including local arrays)
  can't exceed 256 bytes** - the frame copy loops count with the
  6502's 8-bit Y register. Exceeding it is a *compile-time* error
  suggesting the usual fix (make large local arrays global). This
  doubles as a performance guard rail: every call copies the whole
  frame twice, so a huge frame would make every call painfully slow
  anyway.
- **Runaway recursion halts with an on-screen error** instead of
  silently corrupting memory. The runtime checks three exhaustion
  risks on every call: the call stack outgrowing its 4 KB buffer, the
  6502's own 256-byte hardware stack (which holds one `JSR` return
  address per open call - the binding limit for deep recursion, at
  roughly a hundred levels), and the expression operand stack (whose
  entries can now be held across recursive calls - the `n` in
  `n * fact(n - 1)` stays parked for the whole recursion beneath it).
  Any of the three prints a `CC64 RUNTIME ERROR` message and stops.

A software *operand* stack is still used for evaluating nested
expressions (`a * (b + c)` etc.), exactly as before - it's
independent of the call-frame machinery, though as noted above the
two now interact when an operand is held across a recursive call.

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
about it. With that in place, `putchar(x)` and `puts(s)` both
PETSCII-convert at **runtime** (via the same small conversion
routine), so text displays in the **same case you wrote it** whether
it came from a string literal, a buffer you built yourself, or a
single computed character - no flipping, no workaround needed. String
literals are stored as their exact raw bytes (not pre-converted at
compile time) precisely so that this one runtime routine handles
every case uniformly. Plain `char` variables/arrays are **not**
auto-converted (a byte is just a byte until you print it) - only the
print path applies PETSCII mapping.

(This one was actually caught after the fact: my first version didn't
emit the charset switch, so all-uppercase test output rendered as
graphics characters on real hardware. My verification emulator didn't
catch it either, because it was decoding `$C1`-`$DA` as lowercase
unconditionally rather than modeling the two real character sets and
the control code that switches between them - both the compiler and
the emulator are fixed now.)

### Zero-page usage

```
$02/$03  __zpAP   effective-address pointer (array/deref/pointer-index/peek/poke)
$FB/$FC  __zpR    primary register / function return value
$FD/$FE  __zpR2   secondary operand register
```

Only these 6 bytes, and only these. Per the KERNAL's own memory map,
`$02` and `$FB`-`$FE` are the *only* zero-page bytes confirmed
genuinely unused by BASIC/KERNAL. An earlier version of this compiler
also used `$F3`-`$FA` for scratch registers, on the assumption that
they were "adjacent to the known-safe `$FB`-`$FE` range" and therefore
probably fine - they looked free (nothing obviously touches them) but
weren't: `$F3`/`$F4` is the current-line-in-color-RAM pointer, updated
by CHROUT itself whenever it colors a printed character, and `$F5`/
`$F6` is the keyboard-matrix-to-PETSCII conversion table pointer,
touched by the keyboard scan on every background IRQ (~60x/sec)
regardless of what the program does. `puts()` kept its walking pointer
live across multiple `JSR CHROUT` calls in a loop - exactly the
scenario where that collision corrupts memory mid-string and produces
garbage output after the first character. This shipped and was caught
by real-hardware testing, not by the verification here, since the
purpose-built emulator didn't model real KERNAL zero-page usage either
(it now poisons those bytes on every simulated `CHROUT` call
specifically so this class of bug can't slip through silently again).

Everything that doesn't strictly need `(zp),Y` indirection (the old
`__zpAP2`/`__zpT0`/`__zpT1` scratch, and the operand-stack pointer)
now lives in ordinary, non-zero-page RAM instead, which is always safe
since it's memory this program exclusively owns - only zero page is
contested territory. The operand stack itself moved from zero-page
indirect-indexed addressing to plain `absolute,X` addressing (with the
stack index held in a regular byte, not zero page) to make this
possible; it now holds 128 slots instead of 256, which is still far
more than any realistic expression nests.

All compiler-generated assembly symbols (`__fn_`, `__g_`, `__L`, `__rt_`, `__zp`)
live in C's own reserved-identifier namespace (leading double
underscore, or underscore + capital), so they can never collide with
a valid user identifier.

### Calling convention

Arguments are copied into the callee's fixed parameter slots, then a
plain `JSR`. Return values are left in the primary register `__zpR`
at `RTS` time - every `return expr;` just computes `expr` into `__zpR`
and returns; there's no shared epilogue to tear down, since there's
no stack frame to unwind. Pointer parameters/return values use this
exact same mechanism - a pointer is just a 16-bit value like an `int`,
always stored in the full 2-byte slot regardless of what it points to
(a `char*` variable itself takes 2 bytes, even though each byte *it
points to* is 1 byte).

### The standard library

`lib/string.h`: `strlen`, `strcpy`, `strcat`, `strcmp`, `strchr`,
`memset`, `memcpy` - matching the real C library's names and
contracts (including "no bounds checking, ever" - the caller is
responsible for destinations being big enough, exactly like the real
thing) everywhere cc64's type system allows it.

`lib/print.h`: `print_int` (signed decimal), `print_hex` (4 hex
digits, the exact bit pattern regardless of sign), and `newline`.
Deliberately missing: `print_uint`. cc64's `int` is always signed -
there's no unsigned type and no cast operator - so there's no correct
way to decimal-print a 16-bit value in the 32768-65535 range using
cc64's own (always-signed) `/`, `%`, or `<`/`>`; shipping a
`print_uint` built from ordinary cc64 code would silently misprint
exactly that range. `print_hex` doesn't have this problem (see its
comment in `lib/print.h` for why bitwise `&`/`>>` are safe here when
`/` and `%` aren't) and covers most of the same real need - inspecting
a raw 16-bit value - so it's the one shipped instead.

Both headers are **header-only**: `#include` splices their text
directly into your program (see "HOW #include WORKS" in `src/cc64.h`),
so every function you include gets fully compiled into your program
whether you call it or not - there's no linker to strip the unused
ones out. Immaterial for a handful of small functions; worth knowing
if this library ever grows into something bigger.

## Testing

There's no VICE/x64 or other 6502 emulator in this environment, so
verification here was done with a small purpose-built emulator,
`mini6502.py`, that implements exactly the opcode/addressing-mode
subset `cc64` emits and traps `$FFD2` (CHROUT) in software. Since a
real zero-page collision with CHROUT/the KERNAL shipped once already
(see "Zero-page usage" above) without this emulator catching it, it
now also poisons the zero-page bytes CHROUT and the keyboard-scan IRQ
are documented to touch (`$F3`-`$FA`) on every simulated CHROUT call,
so a future regression back into that territory would show up here
instead of only on real hardware:

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
`poke`, `break`/`continue`, and pre/post `++`/`--`. `tests/pointers.c`
covers `&`/`*`, pointer arithmetic scaling (`int*` vs `char*`),
pointer-minus-pointer, unsigned pointer comparisons, arrays decaying
to pointers across a function call, a classic pointer-swap, a
function returning a pointer, and a string literal used as a real
runtime `char*` value copied through a hand-written `copy_str`. Every
computed value in both was checked against hand-calculated expected
output. `tests/recursion.c` covers direct recursion (factorial),
double recursion (fibonacci - an operand held on the expression stack
across an entire recursive subtree), mutual recursion, recursion with
per-level local arrays that must survive inner calls, recursive
pointer walking, 80-deep recursion, and a regression check for a
nested array-target assignment bug found while building the frame
machinery. `tests/include.c` (with the small local header
`tests/testinc.h`) covers both quoted and angle-bracket `#include`,
include-once behavior (the same header is pulled in three times
across the two files - directly, repeated, and via a nested include -
and must not cause a redefinition error), and exercises every
function in `lib/string.h` and `lib/print.h`, including the
sign-extension edge cases in `print_hex` (checked at `-1` and `-4096`,
not just positive values). `tests/forward.c` checks that
forward/backward call references both work regardless of declaration
order. `tests/hello.c` is the basic smoke test for the whole pipeline
(BASIC stub, zero-page init, stack init, `puts`/`putchar`, PETSCII
case mapping).

Run all six:

```sh
for f in hello features forward pointers recursion include; do
    ./cc64 tests/$f.c -o tests/$f.asm
    ./c64asm tests/$f.asm -o tests/$f.prg --listing tests/$f.lst
    python3 mini6502.py tests/$f.prg tests/$f.lst
done
```

## Roadmap (next steps, in a sensible order)

1. ~~Pointers and `&`/`*`~~ - done. Unlocked passing arrays to
   functions, `char*` strings as real runtime values, pointer
   arithmetic, and was the prerequisite for...
2. ~~Function recursion~~ - done, via per-function frame save/restore
   around every call (see "How recursion works" above) rather than a
   full stack-frame rewrite, so the fixed-address storage model and
   all its codegen simplicity survived intact.
3. ~~`#include` and a small standard library~~ - done. Handled
   entirely in the lexer (see "HOW #include WORKS" in `src/cc64.h`),
   which meant zero changes to parsing or codegen; `lib/string.h` and
   `lib/print.h` are ordinary cc64 programs that happen to be headers.
4. `struct`s (a natural pairing with pointers).
5. `do`/`while`, `switch`.
6. Real `printf`-lite - blocked on variadic function support, which
   is a real calling-convention feature (not just a library
   function), unlike the rest of the standard library above.
