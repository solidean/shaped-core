# co-await-temporary

A reference bound to awaiting a call, in a file a `.shaped-lint.yml` denies it in.
The async the call returns is a temporary, and the reference points into its node, which dies with it when nothing else owns it.

The `config=` on a block is the policy it is linted against.

## a reference to awaiting a call is reported

```cpp [co-await-temporary] config="rules:\n  - kind: deny-co-await-temporary\n    value: \"*\"\n    reason: hold the async first\n"
cc::shared_async<int> f() { auto const& rows = co_await load(p); co_return 0; }
```

## a template call is a call

```cpp [co-await-temporary] config="rules:\n  - kind: deny-co-await-temporary\n    value: \"*\"\n    reason: hold the async first\n"
cc::shared_async<int> f() { auto const& rows = co_await find<K>(t, false); co_return 0; }
```

## a held async is not a temporary

```cpp ~[co-await-temporary] config="rules:\n  - kind: deny-co-await-temporary\n    value: \"*\"\n    reason: hold the async first\n"
cc::shared_async<int> f() { auto const pending = load(p); auto const& rows = co_await pending; co_return 0; }
```

## a copy owns its value

```cpp ~[co-await-temporary] config="rules:\n  - kind: deny-co-await-temporary\n    value: \"*\"\n    reason: hold the async first\n"
cc::shared_async<int> f() { auto rows = co_await load(p); co_return 0; }
```

## a getter allowed by name is not reported

```cpp ~[co-await-temporary] config="rules:\n  - kind: deny-co-await-temporary\n    value: \"*\"\n    reason: hold the async first\n  - kind: allow-co-await-temporary\n    value: bytes\n    reason: bytes() returns a copy of a kept handle\n"
cc::shared_async<int> f() { auto const& data = co_await written.bytes(); co_return 0; }
```

## a file nothing was said about is silent

```cpp ~[co-await-temporary]
cc::shared_async<int> f() { auto const& rows = co_await load(p); co_return 0; }
```
