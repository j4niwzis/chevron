# chevron

The XML of streams, for C++23, as modules: the subset of XML 1.0 with
Namespaces that RFC 6120, section 11, allows, read from bytes as they arrive.

- **Events, pulled.** A `chevron::parser` is fed bytes in pieces of any size
  -- split anywhere, inside a tag or a UTF-8 sequence too -- and asked for the
  next event, a `std::variant` of `start_element`, `end_element` and `text`.
  Nothing calls back: the control flow stays in the caller's loop.
  ```cpp
  chevron::parser p;
  p.feed(bytes);
  while (auto next = p.next()) {  // std::expected<std::optional<event>, error>
    if (!*next)
      break;                        // nothing complete yet: feed more
    std::visit(handle, **next);
  }
  ```
- **From any range.** `text | chevron::events` is a view of the events of any
  input range of UTF-8 code units -- a string, a stream read once, a
  generator -- reading only as far as the next event needs; each element is
  a `std::expected<event, error>`.
- **Names resolved.** Element and attribute names come with their namespace
  URI; namespace declarations are not reported as attributes.
- **The subset streams allow, and nothing more.** UTF-8 only; an XML
  declaration at the very start only; comments, processing instructions,
  document type declarations and entity references other than the five
  predefined are errors. Character references are resolved and checked, line
  ends made LF, attribute values normalized, CDATA sections read as text.
- **Limits** on depth, on the size of a token and on the attributes of an
  element, against input meant to exhaust.

What an event refers to -- names, values, text -- stays valid until the next
call.

## Building

CMake 4.3.4 or newer, Ninja, and a compiler that builds C++23 modules with
`import std`: clang 22 or 23 with libc++.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```
