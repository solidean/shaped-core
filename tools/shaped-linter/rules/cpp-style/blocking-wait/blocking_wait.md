# blocking-wait

A call to a blocking wait in a file a `.shaped-lint.yml` denies it in.
The repo denies them across test sources and allows them by name where the wait itself is the subject.

The `config=` on a block is the policy it is linted against.

## a denied call is reported

```cpp [blocking-wait] config="rules:\n  - kind: deny-blocking-wait\n    value: async_blocking_get\n    reason: await it instead\n"
void f() { auto v = cc::async_blocking_get(node); }
```

## an allowed call is not

```cpp ~[blocking-wait] config="rules:\n  - kind: deny-blocking-wait\n    value: async_blocking_get\n    reason: await it instead\n  - kind: allow-blocking-wait\n    value: async_blocking_get\n    reason: the blocking get is the subject\n"
void f() { auto v = cc::async_blocking_get(node); }
```

## a comment naming one is not a call

```cpp ~[blocking-wait] config="rules:\n  - kind: deny-blocking-wait\n    value: async_blocking_get\n    reason: await it instead\n"
// cc::async_blocking_get used to be here
```

## a file nothing was said about is silent

```cpp ~[blocking-wait]
void f() { auto v = cc::async_blocking_get(node); }
```
