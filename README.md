# ctrace

*[Version française](README.fr.md)*

Source-to-source instrumenter for tracing the execution of a C program step by step.
Based on Clang LibTooling (AST + Rewriter).

```sh
ctrace a.c | gcc -x c - -o a.out     # or: ctrace-cc a.c -o a.out
./a.out                              # trace on stderr
CTRACE_OUT=trace.txt ./a.out         # trace to a file
```

## Trace format

```
demo.c:38   s = add(s, 2);                // s=0     executed code // state BEFORE
  demo.c:6  -> add(a=0, b=2)                         function entry + parameters
  demo.c:8    return a + b;               // a=0 b=2
  demo.c:8  <- add = 2                               return + value
          => s=2                                     state AFTER (modified variables)
demo.c:49   while (i < 3)                 // i=0
          -> true                                    result of the condition
```

The `//` comment only appears when the statement reads variables, and the `=>`
line only when it modifies some.

Indentation follows the call depth.

## Building

Dependencies: Clang/LLVM 18 with headers (`libclang-18-dev`, `llvm-18-dev` on Debian/Ubuntu).

```sh
make                 # builds ./ctrace
make test            # instruments and runs examples/demo.c
make install         # /usr/local/bin/ctrace and ctrace-cc
```

Other LLVM version: `make LLVM_CONFIG=llvm-config-19 CLANG=clang-19`.

Options after the file are passed to Clang: `ctrace a.c -Iinclude -DDEBUG`.

## What is traced

| Element | Before | After |
|---|---|---|
| expression, assignment, call | variables read | modified variables / lvalues (`x`, `t[i]`, `p->x`, `*p`) |
| declaration with initializer | variables read | declared variables |
| `if`, `while`, `for`, `do`, `switch` | condition variables | `-> true/false` on each evaluation |
| `for` increment | variables | modified variables |
| function entry | parameters | |
| `return` | expression variables | returned value |
| `break`, `continue`, `goto` | line | |

Display depends on the type (decided at instrumentation time from the AST):
integers, `enum`, `bool`, `char`, floating point, pointers (address), fixed-size
arrays (first 8 elements), `char` arrays (bounded string), `struct` one level
deep, `union` (first member).

## How it works

1. Clang parses the file (preprocessor included) and produces the AST.
2. For each function in the main file, the Rewriter inserts calls to the
   `__ct_*` runtime:
   - before/after each statement in a block;
   - in conditions, as a comma expression:
     `if ((pre..., __ct_cv = !!(c), __ct_cond(__ct_cv)))`;
   - in `return` statements: `return (pre..., __ct_r = (e), <- f = v, __ct_r);`;
   - braces added around bodies without `{ }`.
3. No line break is inserted into user code, and a `#line 1 "a.c"` directive
   follows the runtime: gcc errors and debugging point to the original lines.
4. The runtime (`ctrace_runtime.inc`) is copied at the top of the output: no
   library to link. `__ct_depth` is `weak`, so several instrumented `.c` files
   share the same call depth.

Subexpressions that are not certain to be evaluated (`p && p->x = 1`, `?:`) are
not re-evaluated in the "after" state, to avoid dereferencing a null pointer.

## Known limitations (skeleton)

- Pointers are displayed by address only (`char *` included): reading the
  pointed-to string could crash on an uninitialized pointer.
- A statement entirely produced by a macro (`assert(...)`) is traced but not
  broken down.
- Initializations and increments inside conditions are not shown in the
  "after" state for `switch`.
- No thread support (the depth is global).
- Uninitialized variables appear with their indeterminate value when they are
  read (this is the actual state).
- Functions from included headers are not instrumented, only the file passed
  as argument.
