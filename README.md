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

## Typed reading

XML read straight into plain structs, with no document object model between.
The struct says nothing about XML; its schema, found by argument-dependent
lookup beside it, does:

```cpp
namespace chat {
struct message {
  std::string to, from;
  std::optional<std::string> body;
  std::vector<chevron::any> extensions;
};

constexpr auto xml_schema(chevron::type<message>) {
  using namespace chevron::members;
  return chevron::schema<message>()
      .name("urn:example:client", "message")
      .member<"to">(attribute())
      .member<"from">(attribute())
      .member<"extensions">(unknown_children());
}
}

auto m = chevron::read<chat::message>(text | chevron::events);
auto s = chevron::read_one_of<chat::message, chat::presence>(parser);
```

- Member names come from Boost.PFR; `.member<"to">(…)` names a member, and
  `.members(…)` gives one descriptor to every member in order.
- Descriptors: `attribute()`, `child_text()`, `child()`, `text()`,
  `unknown_children()`, and `_` for the default -- each may give the XML name
  and namespace where they are not the member's name and the parent's
  namespace.
- A member the schema does not mention is a child element: read by its own
  schema where its type has one, as text otherwise. `std::optional` may be
  absent; `std::vector` may repeat.
- Checked at compile time: one descriptor per member, no member described
  twice, and a descriptor that fits its member's type.
- Errors say what and where: a missing attribute or child, a value that is
  not one, an element other than the one asked for, input that ran out, or
  the parser's own error.


## Building

CMake 4.3.4 or newer, Ninja, and a compiler that builds C++23 modules with
`import std`: clang 22 or 23 with libc++. Boost.PFR and, for the tests,
googletest come through cmake-everywhere.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```
