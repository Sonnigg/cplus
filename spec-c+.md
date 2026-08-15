# DATE TABLE (YYYY-MM-DD)
| Feature                         | Added      | Last Modified | Status                         |
|---------------------------------|------------|---------------|--------------------------------|
| STATUS                          | 2026-08-01 | 2026-08-15    | IN-USE                         |
| CMPL__PTR_SIZE                  | 2026-08-03 | 2026-08-03    | IN-USE                         |
| switch                          | 2026-08-03 | 2026-08-03    | IN-USE                         |
| switch::precedence              | 2026-08-03 | 2026-08-03    | IN-USE                         |
| switch::corner_cases            | 2026-08-03 | 2026-08-03    | IN-USE                         |
| structs                         | 2026-08-03 | 2026-08-03    | IN-USE                         |
| structs::methods                | 2026-08-03 | 2026-08-03    | IN-USE                         |
| enums                           | 2026-08-03 | 2026-08-03    | IN-USE                         |
| enums::variants                 | 2026-08-03 | 2026-08-03    | IN-USE                         |
| defer                           | 2026-08-05 | 2026-08-07    | IN-USE                         |
| defer::lowering                 | 2026-08-07 | 2026-08-07    | IN-USE                         |
| defer::loops                    | 2026-08-07 | 2026-08-07    | IN-USE                         |
| defer::multiple                 | 2026-08-07 | 2026-08-07    | IN-USE                         |
| defer::goto                     | 2026-08-07 | 2026-08-07    | IN-USE                         |
| namespace                       | 2026-08-07 | 2026-08-07    | IN-USE                         |
| namespace::edge_cases           | 2026-08-07 | 2026-08-07    | IN-USE                         |
| type-lowering                   | 2026-08-07 | 2026-08-07    | IN-USE                         |
| type-lowering::bool             | 2026-08-07 | 2026-08-07    | IN-USE                         |
| types                           | 2026-08-07 | 2026-08-07    | IN-USE                         |
| forward-declarations            | 2026-08-14 | 2026-08-15    | IN-USE (previously REMOVED)    |

# STATUS - C+26-08-15 standard (1st August 2026)

# Information regarding "(DATE)" (3rd August 2026)
Every header (#, ##, and ###) in the Markdown documentation has the date of its last modification appended in parentheses.

## About the macro CMPL__PTR_SIZE (3rd August 2026)
On every non-Windows OS, this would be the generated version in C code of what CMPL__PTR_SIZE is.
```c
#ifndef CMPL__PTR_SIZE
#if defined(__x86_64__) || defined(__aarch64__) || defined(__riscv64) || defined(__loongarch64)
#define CMPL__PTR_SIZE 8
#elif defined(__i386__) || defined(__arm__) || defined(__riscv) || defined(__mips__) || defined(__powerpc__)
#define CMPL__PTR_SIZE 4
#elif defined(__MSP430__) || defined(__AVR__) || defined(__m16c__) || defined(__RL78__)
#define CMPL__PTR_SIZE 2
#else
#define CMPL__PTR_SIZE 4
#endif
#endif
```

On Windows, this would be the generated version in C code of what CMPL__PTR_SIZE is.
```c
#ifndef CMPL__PTR_SIZE
#if defined(_WIN64) || defined(__x86_64__)
#define CMPL__PTR_SIZE 8
#elif defined(_WIN16) || defined(__I86__)
#define CMPL__PTR_SIZE 2
#else
#define CMPL__PTR_SIZE 4
#endif
#endif
```

When generating C code without explicit target information, the generated fallback defaults to `CMPL__PTR_SIZE == 4` (32-bit). This fallback exists for compatibility and should not be interpreted as support for architectures with non-standard pointer sizes.

## About C+'s switch (3rd August 2026)
C+'s switch does not introduce performance loss and lowers into GNU (specifically TinyCC as it being the backend) compatible if-statements. There are multiple things that are given by C+ and are standardized.

The switch value placeholder (_ for your switch value)
```cx
switch (some_value)
{
    case _:
        foo(); // will always execute since _ is the switch value placeholder and is correctly lowered to __switch_value
        // whereas if you use some_value, it won't be lowered correctly
    default:
        bar(); // never executes
}
```
> _ is the switch value placeholder and is used everywhere you want your switch value to be used, e.g. in the next predicate case (predicate functions).

The predicate functions (lightweight pattern matching)
```cx
switch (some_value)
{
    case foo(_): // this will be lowered into a check if the function outputs something truthy, meaning only if foo(_) is true will this execute
        bar();
    default:
        baz();
}
```

The range operator (INEX and ININ)
```cx
switch (some_value)
{
    case 'a' => 'z': // INEX, lowers to __switch_value >= 'a' && __switch_value < 'z'
        foo();
    case 'a' ==> 'z': // ININ, lowers to __switch_value >= 'a' && __switch_value <= 'z'
        bar();
    default:
        baz();
}
```
> INEX stands for "INclusive-EXclusive", whereas ININ stands for "INclusive-INclusive".
> INEX == \[...\), ININ == \[...\]

The alternative operator (Shares syntax with the logical OR)
```cx
switch (some_value)
{
    case 1 || 2 || 4 || 8 || 16 || 32: // lowers to consecutive __switch_value == 1 || __switch_value == 2 || __switch_value == 4 || ...
        std::puts("Power of 2 under 6!"); // assuming #include <io> was used
    default:
        std::puts("Something else!");
}
```
> When designing the syntax, I thought about using '|' or ',' instead but ultimately decided on '||' since it already stands for an alternative (OR), therefore case 1 || 2 || 4 reads as "case 1 OR 2 OR 4".

The must-satisfy operator (Shares syntax with the logical AND)
```cx
switch (some_value)
{
    case _ > 0 && _ < 10: // lowers to consecutive __switch_value > 0 && __switch_value < 10 ...
        std::puts("Above 0 and below 10!"); // assuming #include <io> was used
    default:
        std::puts("Not above 0 nor below 10!");
}
```
> When designing the syntax, I thought about using '&' instead but ultimately decided on '&&' out of the same reason I chose '||' for the alternative, as it reads naturally as "case some_value > 0 AND some_value < 10"

### Regarding the precedence of operations in `case` (3rd August 2026)
The precedence is as follows:

    Highest

        ()

        predicate functions

        _ (underscore)

        literal equality

        =>   ==>

        &&

        ||

    Lowest

### Regarding corner cases (3rd August 2026)
C+'s switch is a first-to-match-wins switch, that means whatever case matches first will be executed, therefore all other code afterwards is unreachable.
`default` is only allowed at the end of a switch statement and is an optional case (functions the same as `case _:`). Whenever `case _:` is used, every other branch after it becomes unreachable.

> Note that `case _:` becomes `if (__switch_value == __switch_value) { ... }`, aka `if (true) { ... }`.


## About C+'s structs (3rd August 2026)
A C+ struct can be declared the same way a C struct is declared, though there are 3, and only 3, different ways of declaring a struct in C+.

anonymous struct with typedef
```cx
typedef struct
{
    int thing;
} Thing;
```
> Note that the compiler will fail if you use a non-anonymous struct with typedef in C+.

named struct with semicolon after the closing brace
```cx
struct Thing
{
    int thing;
};
```
> Note that C+ automatically emits a typedef for all structs and enums, therefore `struct Thing` will be lowered into a typedef.

named struct without semicolon after the closing brace
```cx
struct Thing
{
    int thing;
}
```
> Note that the semicolon is optional as the C+ compiler will automatically add a semicolon if it's missing.

A struct can contain both fields and methods. C+ defines a method differently than a C++ method on objects. A method is either a static or non-static method, and depending on that, its behaviour slightly changes. A static method requires the `static` keyword and has no other requirement. A non-static method requires the first parameter to be a pointer to its struct (its struct referring to the struct the method is under).

static and non-static method example
```cx
struct Thing
{
    int thing;

    static Thing new()
    {
        return Thing {
            thing: 0,
        };
    }

    void set(Thing *self, int t)
    {
        self->thing = t;
    }
}
```
> The first parameter's name doesn't have any restrictions on what it has to be named, that is entirely up to the programmer.

Methods are called depending on if they are static or not. If the method (assuming a type called T) being called is declared `static`, it can only be called via T::method_name(arguments), though if it were a non-static method, it can only be called via object.method_name(arguments_except_first_parameter).

static and non-static method call example
```cx
// assuming the above-made struct exists in this codebase for this example

int main()
{
    Thing t = Thing::new(); // static method call following T::method_name(arguments), though new doesn't expect any arguments
    t.set(2);               // non-static method call following object.method_name(arguments_except_first_parameter)
    return 0;
}
```
> t.set(2) automatically promotes 't' to a pointer, unless it already is a pointer of depth 1 or more, then it keeps the pointer depth the same, meaning if t was declared `Thing **`, set would receive set(t, 2) aka set(Thing **, int) even though the function was defined as set(Thing *, int).

## About C+'s enums (3rd August 2026)
A C+ enum can be declared the same way a C enum is declared, though there are 3, and only 3, different ways of declaring a enum in C+.

anonymous enum with typedef
```cx
typedef enum
{
    thing
} Thing;
```
> Note that the compiler will fail if you use a non-anonymous enum with typedef in C+.

named enum with semicolon after the closing brace
```cx
enum Thing
{
    thing
};
```
> Note that C+ automatically emits a typedef for all structs and enums, therefore `enum Thing` will be lowered into a typedef.

named enum without semicolon after the closing brace
```cx
enum Thing
{
    thing
}
```
> Note that the semicolon is optional as the C+ compiler will automatically add a semicolon if it's missing.

An enum can only contain so-called "enumerands" or "variants" and an enum in C+ is always scoped. Ultimately a C+ enum is the same as a C enum, different words for numbers, therefore C+ does not have algebraic data types (sum types, Rust enum). When lowering C+ to C, `Thing::thing` (assuming it being a valid variant in the enum `Thing`) will be lowered to Thing_thing. C+ enums let you access their variants through the static-struct or namespace access.

accessing a variant of an enum
```cx
enum Thing
{
    thing1,
    thing2,
}

int main()
{
    Thing::thing1;
}
```
> Thing::thing1 would be 0, though this can be overriden by using VARIANT = INTEGER instead.

## About C+'s defer (5th August 2026)
C+ introduces `defer` into low-level systems programming which acts as almost a manual RAII approach. When defer is used, following the below shown syntax, the code in the "defer block" (`defer { ... }`, inside the curly braces is what the "defer block" refers to) is moved to the end of the lexical scope directly above the closing brace and also moved right before every possible `return` in the scope or child-scopes.

`defer` syntax
```cx
defer {
    foo(); // cleanup code following the above specified rules
}
```
> `defer foo();` will **not** work in any way, as `defer` strictly requires a scope after it.

### Regarding defer's lowering (7th August 2026)
`defer` writes its code block right before any return in its scope or child-scopes.

`defer` lowering example
```cx
#include <io>
void bar()
{
    SomeThing p = SomeThing::new();
    defer {
        p.die();
        std::puts("Cleaned up!");
    }

    if (!p.data)
    {
        return;
    }
    foo(p);
}
```
is lowered into
```c
// libc+/io.hp inlined here
void bar()
{
    SomeThing p = SomeThing_new();
    if (!p.data)
    {
        { p.die(); std_puts("Cleaned up!"); } return;
    }
    foo(p);
    p.die();
    std_puts("Cleaned up!");
}
```
> Note that if bar had a return even after foo, the defer block would be written before it like in the conditional example, yet the non-block-scoped defer code would still be appended afterwards for these cases where no `return;` is given at the end of a void function.

### Regarding defer and loops (7th August 2026)
C+ does not declare `defer` in loops as undefined behaviour, but uses the 2 rules it has already established:
1. emit at the lexical scope's end
2. emit before any returns in the scope or children scopes.

Meaning `defer` does not care if a break or continue is used, it does not put itself before that and only puts itself in front of the closing brace of its scope or any return in its scope or children scopes.

`defer` in nested loops, specifically the loop below, depends entirely on the loop structure. The loop below is an example of a "run this loop, then another loop inside of it"-loop.

generic short example
```cx
while (a)
{
    defer {
        a.destruct();
    }

    while (b)
    {
        defer {
            b.destruct();
        }

        break;
    }
}
```
lowers into
```c
while (a)
{
    while (b)
    {
        break;
        TypeOfB_destruct(&b);
    }
    TypeOfA_destruct(&a);
}
```

That means b.destruct() does not run, but a.destruct() will run as the loop `while (a)` is not exited before calling a.destruct().

### Regarding multiple defer's (7th August 2026)
C+ uses a LIFO approach to defer-unwinding, that means whichever defer was last put into the defer stack is the first defer to be emitted.

### Regarding goto jumps and defer (7th August 2026)
C+'s defer also does not analyze goto in any way and simply follows all of the above mentioned rules.

example of `defer` and `goto`
```cx
void foo(bool a)
{
    defer {
        bar();
    }

    if (a)
        goto somewhere;
    else
        goto nowhere;

    somewhere:
    return;

    nowhere:
    {
        baz();
    }
}
```
lowers into
```c
void foo(unsigned char a)
{
    if (a)
        goto somewhere;
    else
        goto nowhere;
    
    somewhere:
    { bar(); }
    return;

    nowhere:
    {
        baz();
    }
    bar();
}
```

## About C+'s namespace (7th August 2026)
C+ gives the opportunity to use namespaces to organise code and larger programs or libraries. Never do namespaces introduce any kind of overhead at runtime, as they are lowered into a predictable known C-layout.

example of `namespace` and its lowering
```cx
#include <io>

namespace mylib
{
    void sayHelloFromMyLib()
    {
        std::puts("Hello from myLib!");
    }
}
```
lowers into
```c
// libc+/io.hp inlined
    void mylib_sayHelloFromMyLib()
    {
        std_puts("Hello from myLib!");
    }
```

### Regarding C+'s edge-case with any and all namespace-like behaviour and #include (7th August 2026)
C+ itself does not forbid using #include "..." or #include <...> inside of `namespace`, `struct` or `enum`, though it is undefined behaviour, even though the lowering would stay the same.

## About C+'s type lowering (7th August 2026)
C+ has a few types many of which are also found in C. C+'s types are as follows:

    void

    char

    unsigned char

    bool

    short

    unsigned short

    int

    unsigned int

    long

    unsigned long

    long long

    unsigned long long

    float

    double

Though C+'s `bool` type lowers into an architecture-dependent type entirely dependent on the native pointer size (explained and specified in the "Regarding bool" section, types::bool). All other types are lowered 1:1 to standard C types.

### Regarding bool (7th August 2026)
`bool` lowers into `unsigned T` where T is dependent on the native pointer size, generally for 64-bit targets bool lowers into `unsigned int`, 32-bit into `unsigned short` and 16-bit into `unsigned char`, and C+ does not forbid using any value in the 0-T_max range as a boolean value, though typically C+ uses and encourages to use `true` (lowering into 1) and `false` (lowering into 0).

examples of `bool` that would be allowed
```cx
bool b1 = 243;
bool b2 = 19 ;
bool b3 = 43 ;
```
> note that this could also be used to do arithmetic with true and false and even to use bool as a static_cast target for pointer normalisation.

C+'s types.hp (libc+) introduces `boolN_t` types for `bool1_t`, `bool2_t`, `bool4_t`, and `bool8_t` where N determines the number of bytes the bool type occupies, respectively, `bool` can be either `bool1_t`, `bool2_t`, or `bool4_t`, though as mentioned this depends on the native pointer size.

examples of `boolN_t`
```cx
std::bool1_t b1 = true; // std::bool1_t == std::uint8_t
std::bool2_t b2 = true; // std::bool2_t == std::uint16_t
std::bool4_t b4 = true; // std::bool4_t == std::uint32_t
std::bool8_t b8 = true; // std::bool8_t == std::uint64_t
```
> Note that `boolN_t` names the number of bytes it occupies, specified by N, whereas `intN_t` and `uintN_t` name the number of bits they occupy, specified by N.

## About C+'s types in libc+ and builtin (7th August 2026)
C+ defines 14 types as its builtin types, though 1 of which (`bool`) is a compiler-macro depending on the architecture as declared and define in type-lowering and type-lowering::bool. libc+ declares and defines types in various files, one of the prominent ones is `types.hp` which defines 16 types, those being the following with examples.

examples of the 16 `types.hp` defined types
```cx
std::int8_t  i1; // signed 8-bit  integer
std::int16_t i2; // signed 16-bit integer
std::int32_t i4; // signed 32-bit integer
std::int64_t i8; // signed 64-bit integer

std::uint8_t  u1; // unsigned 8-bit  integer
std::uint16_t u2; // unsigned 16-bit integer
std::uint32_t u4; // unsigned 32-bit integer
std::uint64_t u8; // unsigned 64-bit integer

std::bool8_t b8; // unsigned 64-bit integer
std::bool4_t b4; // unsigned 32-bit integer
std::bool2_t b2; // unsigned 16-bit integer
std::bool1_t b1; // unsigned 8-bit  integer

#if CMPL__PTR_SIZE == 8
    typedef uint64_t ptrsize_t;  // unsigned 64-bit integer
    typedef int64_t  sptrsize_t; //   signed 64-bit integer

#elif CMPL__PTR_SIZE == 4
    typedef uint32_t ptrsize_t;  // unsigned 32-bit integer
    typedef int32_t  sptrsize_t; //   signed 32-bit integer

#elif CMPL__PTR_SIZE == 2
    typedef uint16_t ptrsize_t;  // unsigned 16-bit integer
    typedef int16_t  sptrsize_t; //   signed 16-bit integer

#endif
    typedef ptrsize_t  size_t;  // depending on the above declared  ptrsize_t
    typedef sptrsize_t ssize_t; // depending on the above declared sptrsize_t
```

## About C+'s forward declarations (14th and 15th August 2026)
Forward declarations in C+ are considered unstandardized as they are compiler-dependent. The original C+ Compiler (c+, cc+, cplus) by `Ben Samberg` (alias `Sonnigg`) does not declare forward declarations to be usable everywhere. Generally speaking though, C+ does not standardize forward declarations and instead depends on the used C+ compiler as-to how they are handled and lowered into C or other targets.