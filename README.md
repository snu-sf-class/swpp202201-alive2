Alive2
======

Alive2 consists of several libraries and tools for analysis and verification
of LLVM code and transformations.  
This fork is modified to pre-convert the SWPP2022-specific intrinsics.
See [here](https://github.com/AliveToolkit/alive2) for the original repo

For a technical introduction to Alive2, please see [our paper from
PLDI 2021](https://web.ist.utl.pt/nuno.lopes/pubs.php?id=alive2-pldi21).


WARNING
-------
Alive2 does not support inter-procedural transformations. Alive2 may crash
or produce spurious counterexamples if run with such passes.


Prerequisites
-------------
To build Alive2 you need recent versions of:
* cmake
* gcc/clang
* re2c
* Z3
* LLVM (With RTTI and exception handling enabled: the build script in class repo already does that)
* hiredis (optional, needed for caching)


Building
--------

Use the `install-alive2.sh` in the class repo. Simply replace the git repository URL should work.


Running the Standalone Translation Validation Tool (alive-tv)
--------

This tool has two modes.

In the first mode, specify a source (original) and target (optimized)
IR file. For example, let's prove that removing `nsw` is correct
for addition:

```
$ ./alive-tv src.ll tgt.ll

----------------------------------------
define i32 @f(i32 %a, i32 %b) {
  %add = add nsw i32 %b, %a
  ret i32 %add
}
=>
define i32 @f(i32 %a, i32 %b) {
  %add = add i32 %b, %a
  ret i32 %add
}

Transformation seems to be correct!
```

Flipping the inputs yields a counterexample, since it's not correct, in general,
to add `nsw`.
If you are not interested in counterexamples using `undef`, you can use the
command-line argument `-disable-undef-input`.

In the second mode, specify a single unoptimized IR file. alive-tv
will optimize it using an optimization pipeline similar to -O2, but
without any interprocedural passes, and then attempt to validate the
translation.

For example, as of February 6 2020, the `release/10.x` branch contains
an optimizer bug that can be triggered as follows:

```
$ cat foo.ll
define i3 @foo(i3) {
  %x1 = sub i3 0, %0
  %x2 = icmp ne i3 %0, 0
  %x3 = zext i1 %x2 to i3
  %x4 = lshr i3 %x1, %x3
  %x5 = lshr i3 %x4, %x3
  ret i3 %x5
}
$ ./alive-tv foo.ll

----------------------------------------
define i3 @foo(i3 %0) {
  %x1 = sub i3 0, %0
  %x2 = icmp ne i3 %0, 0
  %x3 = zext i1 %x2 to i3
  %x4 = lshr i3 %x1, %x3
  %x5 = lshr i3 %x4, %x3
  ret i3 %x5
}
=>
define i3 @foo(i3 %0) {
  %x1 = sub i3 0, %0
  ret i3 %x1
}
Transformation doesn't verify!
ERROR: Value mismatch

Example:
i3 %0 = #x5 (5, -3)

Source:
i3 %x1 = #x3 (3)
i1 %x2 = #x1 (1)
i3 %x3 = #x1 (1)
i3 %x4 = #x1 (1)
i3 %x5 = #x0 (0)

Target:
i3 %x1 = #x3 (3)
Source value: #x0 (0)
Target value: #x3 (3)

Summary:
  0 correct transformations
  1 incorrect transformations
  0 errors
```

Please keep in mind that you do not have to compile Alive2 in order to
try out alive-tv; it is available online: https://alive2.llvm.org/ce/


Running the Standalone LLVM Execution Tool (alive-exec)
--------

This tool uses Alive2 as an interpreter for an LLVM function. It is
currently highly experimental and has many restrictions. For example,
the function cannot take inputs, cannot use memory, cannot depend on
undefined behaviors, and cannot include loops that execute too many
iterations.

Caching
--------

The alive-tv tool and the Alive2 translation validation opt plugin
support using an external Redis server to avoid performing redundant
queries. This feature is not intended for general use, but rather to
speed up certain systematic testing workloads that perform a lot of
repeated work. When it hits a repeated refinement check, it prints
"Skipping repeated query" instead of performing the query.

If you want to use this functionality, you will need to manually start
and stop, as appropriate, a Redis server instance on localhost. Alive2
should be the only user of this server.

LLVM Bugs Found by Alive2
--------

[BugList.md](BugList.md) shows the list of LLVM bugs found by Alive2.
