# chevron

The XML of streams, for C++26, as modules: the subset of XML 1.0 with
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
- **From any range, never ahead.** `text | chevron::events` is a view of the
  events of any input range of UTF-8 code units -- a string, a stream read
  once, a socket, a generator -- read unit by unit up to the `<` or `>`
  where an event can end, and asked whether it has ended only where another
  unit is needed: over a socket, nothing waits for bytes the next event does
  not need. Each element is a `std::expected<event, error>`.
- **Or in chunks.** A range of chunks -- what each read of a socket brought,
  say -- is taken a chunk at a time: `pieces | chevron::events`. Where
  reading ahead costs nothing, `chevron::chunked<N>` makes chunks of up to
  N units: `file | chevron::chunked<4096> | chevron::events`.
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

## Typed reading and writing

XML read straight into plain structs, and written from them, with no document
object model between.
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
  `unknown_children()`, `unknown_attributes()` (a `chevron::kept_attributes`
  for every attribute nothing else claims), and `_` for the default -- each may give the XML name
  and namespace where they are not the member's name and the parent's
  namespace.
- A member the schema does not mention is a child element: read by its own
  schema where its type has one, as text otherwise. `std::optional` may be
  absent; `std::vector` may repeat.
- `chevron::tagged<A, B, …, chevron::any>` for a child that is one of several
  types, chosen by its element name as each alternative's schema gives it
  (with its `.when<>()`), and read straight into that type -- no tree on the
  way. `chevron::any`, allowed only last, takes an element no alternative
  names, and is the only tree there is. `is<T>()`, `as<T>()`, `data()`;
  optional and vector as for any member, and written back as it was held.
  Members claim children in their order: a `tagged` ending in `any` takes
  every child the members before it have not.
- Checked at compile time: one descriptor per member, no member described
  twice, and a descriptor that fits its member's type.
- Writing is the same schema the other way, lazily:
  `chevron::to_xml(value) | std::ranges::to<std::string>()` -- a view of the
  document's characters made as they are pulled, piece by piece, with
  `.chunks()` for the pieces themselves; or at once, with
  `chevron::write(out, value)` to an output iterator of `char` -- namespaces
  declared where they change, values escaped, what was kept as
  `chevron::any` written back as it came, and what is written read back as
  the same value.
- Errors say what and where: a missing attribute or child, a value that is
  not one, an element other than the one asked for, input that ran out, or
  the parser's own error.

All of it is `constexpr`: a document is written with `to_xml` and read back
with `read` inside a `static_assert` in the tests.

## Speed

Character data is taken in runs where the text is in memory: ASCII that
stands for itself is found 32 bytes at a time, with Clang's vector types (SSE
or AVX on x86, NEON on ARM, from the same code), and the rest is checked as
UTF-8 16 bytes at a time with the lookup method simdjson uses (SSSE3, chosen
at run time), then appended in one piece. The text an event refers to is kept
in blocks that are used again, so a parser that has run a while allocates
nothing more for it.

`test/bench/text.cc`, a stanza with a body of 16 KB read through
`chevron::events`, one core: 308 MB/s for ASCII, 289 MB/s for Cyrillic.

Tested under AddressSanitizer and UndefinedBehaviorSanitizer as well.


## Building

CMake 4.3.4 or newer, Ninja, and a compiler that builds C++26 modules with
`import std`: clang 22 or 23 with libc++. Boost.PFR and, for the tests,
googletest come through cmake-everywhere.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

## Licence

GNU Affero General Public License, version 3 only (`AGPL-3.0-only`) -- the
text is in `LICENSE`. A program that uses this library is a work based on
it; whoever interacts with such a program over a network is offered its
source, as the licence's section 13 says.
