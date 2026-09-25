// Typed reading and writing: XML read straight into plain structs, and
// written from them, with no document object model between. A struct says nothing about XML; its schema, found
// by argument-dependent lookup beside it, does:
//
//   namespace chat {
//   struct message { std::string to; std::optional<std::string> body; };
//   constexpr auto xml_schema(chevron::type<message>) {
//     using namespace chevron::members;
//     return chevron::schema<message>().name("urn:example:client", "message")
//                                      .member<"to">(attribute());
//   }
//   }
//   auto m = chevron::read<chat::message>(text | chevron::events);
//
// Member names come from Boost.PFR. A member a schema does not mention is a
// child element: by its own schema where its type has one, holding text
// where it does not; std::optional may be absent, std::vector may repeat.
export module chevron.typed;

import std;
import boost.pfr;
import chevron.parser;

export namespace chevron {

// The tag a schema is looked up by: xml_schema(chevron::type<T>) in T's
// namespace.
template <class T>
struct type {};

// A string as a template argument, for .member<"name">.
template <std::size_t N>
struct fixed_string {
  char text[N]{};
  constexpr fixed_string(const char (&from)[N]) { std::copy_n(from, N, text); }
  constexpr std::string_view view() const { return {text, N - 1}; }
};

// A subtree kept as it came, for what a schema does not describe.
struct any_node;
struct any {
  std::string uri;
  std::string local;
  std::vector<std::pair<std::pair<std::string, std::string>, std::string>> attributes;  // {uri, local}, value
  std::vector<any_node> children;
};
struct any_node {
  std::variant<any, std::string> value;  // an element, or text
};

namespace members {

enum class kind : std::uint8_t { deduced, attribute, child_text, child, text, unknown_children };

// What a member is in XML, and its name there where it is not the member's.
struct descriptor {
  kind what = kind::deduced;
  std::string_view local{};
  std::optional<std::string_view> uri{};
};

// An attribute; in no namespace unless one is given.
constexpr descriptor attribute(std::string_view local = {}, std::optional<std::string_view> uri = {}) {
  return {kind::attribute, local, uri};
}
// A child element holding only text: <body>…</body>. In the parent's
// namespace unless one is given.
constexpr descriptor child_text(std::string_view local = {}, std::optional<std::string_view> uri = {}) {
  return {kind::child_text, local, uri};
}
// A child element read by its own type's schema.
constexpr descriptor child(std::string_view local = {}, std::optional<std::string_view> uri = {}) {
  return {kind::child, local, uri};
}
// The element's own character data.
constexpr descriptor text() { return {kind::text}; }
// Every child element nothing else claims, kept whole: a std::vector<any>.
constexpr descriptor unknown_children() { return {kind::unknown_children}; }
// The default, spelled out: a child element, by schema or with text.
inline constexpr descriptor _{};

}  // namespace members

// The description of how a T is read: built with .name(), .members() and
// .member<"name">(), at compile time.
template <class T>
struct schema_t {
  static constexpr std::size_t count = boost::pfr::tuple_size_v<T>;

  std::string_view uri{};
  std::string_view local{};
  bool named = false;
  std::array<members::descriptor, count> bound{};
  std::array<bool, count> said{};

  // The element's name: its namespace and its local part.
  constexpr schema_t name(std::string_view in, std::string_view local_part) const {
    schema_t out = *this;
    out.uri = in;
    out.local = local_part;
    out.named = true;
    return out;
  }

  // Every member, in order: one descriptor each.
  template <class... D>
  constexpr schema_t members(D... each) const {
    static_assert(sizeof...(D) == count, "chevron: .members() needs one descriptor for every member");
    schema_t out = *this;
    std::size_t at = 0;
    (out.set(at++, members::descriptor(each)), ...);
    return out;
  }

  // One member, by its name in C++.
  template <fixed_string Name>
  constexpr schema_t member(members::descriptor what) const {
    constexpr std::size_t at = index_of(Name.view());
    static_assert(at < count, "chevron: .member<\"...\">() names no member of this type");
    schema_t out = *this;
    out.set(at, what);
    return out;
  }

  static constexpr std::size_t index_of(std::string_view wanted) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    for (std::size_t at = 0; at < names.size(); ++at)
      if (names[at] == wanted)
        return at;
    return count;
  }

 private:
  constexpr void set(std::size_t at, members::descriptor what) {
    if (said[at])
      throw "chevron: a member is described twice";
    said[at] = true;
    bound[at] = what;
  }
};

template <class T>
constexpr schema_t<T> schema() {
  return {};
}

// A type with a schema.
template <class T>
concept described = requires { xml_schema(type<T>{}); };

enum class read_code : std::uint8_t {
  parse,              // the XML was not well-formed: see parse_error
  incomplete,         // the input ran out inside the element
  unexpected_element, // an element other than the one asked for
  missing_attribute,  // a required attribute is absent
  missing_child,      // a required child element is absent
  bad_value,          // text that is not a value of the member's type
};

struct read_error {
  read_code code;
  std::string where;                 // the name involved
  std::optional<error> parse_error;  // where code is parse
};

// What can be read from: anything with next() as chevron::parser has it.
template <class S>
concept event_source = requires(S& source) {
  { source.next() } -> std::same_as<std::expected<std::optional<event>, error>>;
};

}  // namespace chevron

namespace chevron::detail::reading {

template <class T> struct is_optional : std::false_type {};
template <class T> struct is_optional<std::optional<T>> : std::true_type { using type = T; };
template <class T> struct is_vector : std::false_type {};
template <class T> struct is_vector<std::vector<T>> : std::true_type { using type = T; };

// The type of one occurrence: what optional<> or vector<> holds.
template <class T>
using element_of = typename std::conditional_t<
    is_optional<T>::value, is_optional<T>,
    std::conditional_t<is_vector<T>::value, is_vector<T>, std::type_identity<T>>>::type;

template <class T>
concept text_like = std::same_as<T, std::string> || std::same_as<T, bool> ||
                    (std::is_arithmetic_v<T> && !std::same_as<T, char>);

template <class T>
constexpr std::optional<T> value_of(std::string_view text) {
  if constexpr (std::same_as<T, std::string>) {
    return std::string(text);
  } else if constexpr (std::same_as<T, bool>) {
    if (text == "true" || text == "1")
      return true;
    if (text == "false" || text == "0")
      return false;
    return std::nullopt;
  } else {
    T out{};
    const auto [end, problem] = std::from_chars(text.data(), text.data() + text.size(), out);
    if (problem != std::errc{} || end != text.data() + text.size())
      return std::nullopt;
    return out;
  }
}

// Put one occurrence into a member: assigned, emplaced or appended.
template <class Member, class Value>
constexpr void store(Member& member, Value&& value) {
  if constexpr (is_vector<Member>::value)
    member.push_back(std::forward<Value>(value));
  else
    member = std::forward<Value>(value);
}

template <class Source>
constexpr std::expected<event, read_error> next_event(Source& source) {
  auto next = source.next();
  if (!next)
    return std::unexpected(read_error{read_code::parse, {}, next.error()});
  if (!*next)
    return std::unexpected(read_error{read_code::incomplete, {}, std::nullopt});
  return **next;
}

template <class Source>
constexpr std::expected<any, read_error> capture(Source& source, const start_element& start) {
  any out;
  out.uri = start.name.uri;
  out.local = start.name.local;
  for (const attribute& one : start.attributes)
    out.attributes.push_back({{std::string(one.name.uri), std::string(one.name.local)}, std::string(one.value)});
  for (;;) {
    auto next = next_event(source);
    if (!next)
      return std::unexpected(next.error());
    if (const auto* child = std::get_if<start_element>(&*next)) {
      auto inner = capture(source, *child);
      if (!inner)
        return std::unexpected(inner.error());
      out.children.push_back({std::move(*inner)});
    } else if (const auto* piece = std::get_if<text>(&*next)) {
      out.children.push_back({std::string(piece->content)});
    } else {
      return out;
    }
  }
}

template <class Source>
constexpr std::expected<void, read_error> skip(Source& source) {
  for (std::size_t depth = 1; depth > 0;) {
    auto next = next_event(source);
    if (!next)
      return std::unexpected(next.error());
    if (std::holds_alternative<start_element>(*next))
      ++depth;
    else if (std::holds_alternative<end_element>(*next))
      --depth;
  }
  return {};
}

// The text of an element that holds only text, up to its end.
template <class Source>
constexpr std::expected<std::string, read_error> text_content(Source& source, std::string_view where) {
  std::string out;
  for (;;) {
    auto next = next_event(source);
    if (!next)
      return std::unexpected(next.error());
    if (const auto* piece = std::get_if<text>(&*next))
      out += piece->content;
    else if (std::holds_alternative<end_element>(*next))
      return out;
    else
      return std::unexpected(read_error{read_code::bad_value, std::string(where), std::nullopt});
  }
}

template <class T, class Source>
constexpr std::expected<T, read_error> read_element(Source& source, const start_element& start);

template <class T>
struct described_member {
  static constexpr auto schema = xml_schema(type<T>{});
  static constexpr auto names = boost::pfr::names_as_array<T>();

  template <std::size_t I>
  using member_type = std::remove_cvref_t<decltype(boost::pfr::get<I>(std::declval<T&>()))>;

  // What member I is, the default resolved.
  template <std::size_t I>
  static constexpr members::kind kind_of() {
    constexpr members::kind said = schema.bound[I].what;
    if constexpr (said != members::kind::deduced) {
      return said;
    } else {
      using one = element_of<member_type<I>>;
      static_assert(described<one> || text_like<one>,
                    "chevron: a member with no descriptor must have a schema, or hold text");
      return described<one> ? members::kind::child : members::kind::child_text;
    }
  }

  template <std::size_t I>
  static constexpr std::string_view local_of() {
    return schema.bound[I].local.empty() ? names[I] : schema.bound[I].local;
  }

  // A child element's name: its local part, and its namespace where it is not
  // the parent's.
  template <std::size_t I>
  static constexpr std::pair<std::string_view, std::optional<std::string_view>> child_name() {
    using one = element_of<member_type<I>>;
    constexpr members::descriptor said = schema.bound[I];
    if constexpr (kind_of<I>() == members::kind::child) {
      constexpr auto inner = xml_schema(type<one>{});
      if constexpr (inner.named)
        return {said.local.empty() ? inner.local : said.local,
                said.uri ? said.uri : std::optional<std::string_view>(inner.uri)};
    }
    return {local_of<I>(), said.uri};
  }

  template <std::size_t I>
  static constexpr void check() {
    using held = member_type<I>;
    using one = element_of<held>;
    constexpr members::kind what = kind_of<I>();
    if constexpr (what == members::kind::attribute || what == members::kind::text)
      static_assert(text_like<one> && !is_vector<held>::value,
                    "chevron: an attribute() or text() member must hold one value");
    else if constexpr (what == members::kind::child_text)
      static_assert(text_like<one>, "chevron: a child_text() member must hold text or a value");
    else if constexpr (what == members::kind::child)
      static_assert(described<one>, "chevron: a child() member's type needs a schema");
    else if constexpr (what == members::kind::unknown_children)
      static_assert(std::same_as<held, std::vector<any>>,
                    "chevron: unknown_children() needs a std::vector<chevron::any>");
  }
};

template <class T, class Source>
constexpr std::expected<T, read_error> read_element(Source& source, const start_element& start) {
  using info = described_member<T>;
  constexpr std::size_t count = info::schema.count;
  T out{};
  std::optional<read_error> failure;
  const std::string parent_uri(start.name.uri);

  // Attributes first: the start element's views last only until the next event.
  [&]<std::size_t... I>(std::index_sequence<I...>) {
    (info::template check<I>(), ...);
    const auto one = [&]<std::size_t K>(std::integral_constant<std::size_t, K>) {
      if constexpr (info::template kind_of<K>() == members::kind::attribute) {
        if (failure)
          return;
        const std::string_view local = info::template local_of<K>();
        const std::string_view uri = info::schema.bound[K].uri.value_or(std::string_view());
        auto& member = boost::pfr::get<K>(out);
        using held = std::remove_cvref_t<decltype(member)>;
        for (const attribute& found : start.attributes)
          if (found.name.local == local && found.name.uri == uri) {
            const auto value = value_of<element_of<held>>(found.value);
            if (!value)
              failure = read_error{read_code::bad_value, std::string(local), std::nullopt};
            else
              member = *value;
            return;
          }
        if constexpr (!is_optional<held>::value)
          failure = read_error{read_code::missing_attribute, std::string(local), std::nullopt};
      }
    };
    (one(std::integral_constant<std::size_t, I>{}), ...);
  }(std::make_index_sequence<count>{});
  if (failure)
    return std::unexpected(*failure);

  std::array<bool, count> seen{};
  for (;;) {
    auto next = next_event(source);
    if (!next)
      return std::unexpected(next.error());
    if (std::holds_alternative<end_element>(*next))
      break;
    if (const auto* piece = std::get_if<text>(&*next)) {
      [&]<std::size_t... I>(std::index_sequence<I...>) {
        const auto one = [&]<std::size_t K>(std::integral_constant<std::size_t, K>) {
          if constexpr (info::template kind_of<K>() == members::kind::text) {
            auto& member = boost::pfr::get<K>(out);
            using held = std::remove_cvref_t<decltype(member)>;
            if constexpr (std::same_as<element_of<held>, std::string>) {
              if constexpr (is_optional<held>::value) {
                if (!member)
                  member.emplace();
                *member += piece->content;
              } else {
                member += piece->content;
              }
            } else {
              const auto value = value_of<element_of<held>>(piece->content);
              if (!value)
                failure = read_error{read_code::bad_value, "text", std::nullopt};
              else
                member = *value;
            }
          }
        };
        (one(std::integral_constant<std::size_t, I>{}), ...);
      }(std::make_index_sequence<count>{});
      if (failure)
        return std::unexpected(*failure);
      continue;
    }
    const start_element child = std::get<start_element>(*next);
    const std::string child_uri(child.name.uri);
    const std::string child_local(child.name.local);
    bool claimed = false;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      const auto one = [&]<std::size_t K>(std::integral_constant<std::size_t, K>) {
        constexpr members::kind what = info::template kind_of<K>();
        if constexpr (what == members::kind::child || what == members::kind::child_text) {
          if (claimed || failure)
            return;
          constexpr auto name = info::template child_name<K>();
          const std::string_view uri = name.second.value_or(std::string_view(parent_uri));
          if (child_local != name.first || child_uri != uri)
            return;
          claimed = true;
          seen[K] = true;
          auto& member = boost::pfr::get<K>(out);
          using held = std::remove_cvref_t<decltype(member)>;
          using one_value = element_of<held>;
          if constexpr (what == members::kind::child) {
            auto value = read_element<one_value>(source, child);
            if (!value)
              failure = value.error();
            else
              store(member, std::move(*value));
          } else {
            auto content = text_content(source, child_local);
            if (!content) {
              failure = content.error();
            } else {
              auto value = value_of<one_value>(*content);
              if (!value)
                failure = read_error{read_code::bad_value, child_local, std::nullopt};
              else
                store(member, std::move(*value));
            }
          }
        }
      };
      (one(std::integral_constant<std::size_t, I>{}), ...);
      if (claimed || failure)
        return;
      const auto unknown = [&]<std::size_t K>(std::integral_constant<std::size_t, K>) {
        if constexpr (info::template kind_of<K>() == members::kind::unknown_children) {
          if (claimed || failure)
            return;
          claimed = true;
          auto kept = capture(source, child);
          if (!kept)
            failure = kept.error();
          else
            boost::pfr::get<K>(out).push_back(std::move(*kept));
        }
      };
      (unknown(std::integral_constant<std::size_t, I>{}), ...);
    }(std::make_index_sequence<count>{});
    if (failure)
      return std::unexpected(*failure);
    if (!claimed)
      if (auto skipped = skip(source); !skipped)
        return std::unexpected(skipped.error());
  }

  // What must be there and was not.
  [&]<std::size_t... I>(std::index_sequence<I...>) {
    const auto one = [&]<std::size_t K>(std::integral_constant<std::size_t, K>) {
      constexpr members::kind what = info::template kind_of<K>();
      using held = typename info::template member_type<K>;
      if constexpr ((what == members::kind::child || what == members::kind::child_text) &&
                    !is_optional<held>::value && !is_vector<held>::value)
        if (!seen[K] && !failure)
          failure = read_error{read_code::missing_child, std::string(info::template child_name<K>().first), std::nullopt};
    };
    (one(std::integral_constant<std::size_t, I>{}), ...);
  }(std::make_index_sequence<count>{});
  if (failure)
    return std::unexpected(*failure);
  return out;
}

template <class T>
constexpr bool is_named(const start_element& start) {
  constexpr auto schema = xml_schema(type<T>{});
  return !schema.named || (start.name.uri == schema.uri && start.name.local == schema.local);
}

// A range of events -- std::expected<event, error> each, as text | events
// gives -- as an event source.
template <class Range>
class range_source {
 public:
  constexpr explicit range_source(Range& range)
      : at_(std::ranges::begin(range)), end_(std::ranges::end(range)) {}

  constexpr std::expected<std::optional<event>, error> next() {
    if (started_)
      ++at_;
    started_ = true;
    if (at_ == end_)
      return std::nullopt;
    const std::expected<event, error> one = *at_;
    if (!one)
      return std::unexpected(one.error());
    return *one;
  }

 private:
  std::ranges::iterator_t<Range> at_;
  std::ranges::sentinel_t<Range> end_;
  bool started_ = false;
};

}  // namespace chevron::detail::reading

export namespace chevron {

// One element read into a T: the next event must be its start.
template <described T, event_source Source>
constexpr std::expected<T, read_error> read(Source& source) {
  auto next = detail::reading::next_event(source);
  if (!next)
    return std::unexpected(next.error());
  const auto* start = std::get_if<start_element>(&*next);
  if (!start || !detail::reading::is_named<T>(*start))
    return std::unexpected(read_error{read_code::unexpected_element,
                                      start ? std::string(start->name.local) : std::string(), std::nullopt});
  return detail::reading::read_element<T>(source, *start);
}

// The same, from a range of events.
template <described T, std::ranges::input_range Range>
  requires(!event_source<Range>)
constexpr std::expected<T, read_error> read(Range&& events) {
  detail::reading::range_source<std::remove_reference_t<Range>> source(events);
  return read<T>(source);
}

// One element read into whichever of the types its name is.
template <described... T, event_source Source>
constexpr std::expected<std::variant<T...>, read_error> read_one_of(Source& source) {
  auto next = detail::reading::next_event(source);
  if (!next)
    return std::unexpected(next.error());
  const auto* start = std::get_if<start_element>(&*next);
  std::optional<std::expected<std::variant<T...>, read_error>> out;
  if (start)
    ([&] {
      if (out || !detail::reading::is_named<T>(*start))
        return;
      auto one = detail::reading::read_element<T>(source, *start);
      if (one)
        out.emplace(std::variant<T...>(std::in_place_type<T>, std::move(*one)));
      else
        out.emplace(std::unexpected(one.error()));
    }(), ...);
  if (!out)
    return std::unexpected(read_error{read_code::unexpected_element,
                                      start ? std::string(start->name.local) : std::string(), std::nullopt});
  return std::move(*out);
}

template <described... T, std::ranges::input_range Range>
  requires(!event_source<Range>)
constexpr std::expected<std::variant<T...>, read_error> read_one_of(Range&& events) {
  detail::reading::range_source<std::remove_reference_t<Range>> source(events);
  return read_one_of<T...>(source);
}

}  // namespace chevron

namespace chevron::detail::writing {

using reading::element_of;
using reading::is_optional;
using reading::is_vector;
using reading::described_member;

template <class Out>
constexpr void put(Out& out, std::string_view text) {
  for (const char one : text)
    *out++ = one;
}

// Character data: what would end it or start markup, as references.
template <class Out>
constexpr void put_text(Out& out, std::string_view text) {
  for (const char one : text) {
    if (one == '&')
      put(out, "&amp;");
    else if (one == '<')
      put(out, "&lt;");
    else if (one == '>')
      put(out, "&gt;");
    else if (one == '\r')
      put(out, "&#xD;");
    else
      *out++ = one;
  }
}

// An attribute value in double quotes: white space other than a space as
// references too, which attribute-value normalization would otherwise turn
// into spaces.
template <class Out>
constexpr void put_value(Out& out, std::string_view text) {
  for (const char one : text) {
    if (one == '&')
      put(out, "&amp;");
    else if (one == '<')
      put(out, "&lt;");
    else if (one == '"')
      put(out, "&quot;");
    else if (one == '\t')
      put(out, "&#x9;");
    else if (one == '\n')
      put(out, "&#xA;");
    else if (one == '\r')
      put(out, "&#xD;");
    else
      *out++ = one;
  }
}

template <class T>
constexpr std::string text_of(const T& value) {
  if constexpr (std::same_as<T, std::string>) {
    return value;
  } else if constexpr (std::same_as<T, bool>) {
    return value ? "true" : "false";
  } else {
    std::array<char, 64> digits{};
    const auto [end, problem] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    return std::string(digits.data(), end);
  }
}

// The start of an element: its name, a default namespace declaration where
// it is not the one in effect, and its attributes, each in a namespace with a
// prefix of its own -- xml for the XML namespace, declared otherwise.
template <class Out>
constexpr void open(Out& out, std::string_view uri, std::string_view local, std::string_view in_effect,
                    const std::vector<std::tuple<std::string_view, std::string_view, std::string>>& attributes) {
  *out++ = '<';
  put(out, local);
  if (uri != in_effect) {
    put(out, " xmlns=\"");
    put_value(out, uri);
    *out++ = '"';
  }
  std::size_t prefixes = 0;
  for (const auto& [attribute_uri, attribute_local, value] : attributes) {
    *out++ = ' ';
    if (attribute_uri == std::string_view("http://www.w3.org/XML/1998/namespace")) {
      put(out, "xml:");
    } else if (!attribute_uri.empty()) {
      const std::string prefix = "a" + std::to_string(prefixes++);
      put(out, "xmlns:");
      put(out, prefix);
      put(out, "=\"");
      put_value(out, attribute_uri);
      put(out, "\" ");
      put(out, prefix);
      *out++ = ':';
    }
    put(out, attribute_local);
    put(out, "=\"");
    put_value(out, value);
    *out++ = '"';
  }
}

template <class Out>
constexpr void write_any(Out& out, const any& element, std::string_view in_effect) {
  std::vector<std::tuple<std::string_view, std::string_view, std::string>> attributes;
  for (const auto& [name, value] : element.attributes)
    attributes.emplace_back(name.first, name.second, value);
  open(out, element.uri, element.local, in_effect, attributes);
  if (element.children.empty()) {
    put(out, "/>");
    return;
  }
  *out++ = '>';
  for (const any_node& child : element.children) {
    if (const auto* text = std::get_if<std::string>(&child.value))
      put_text(out, *text);
    else
      write_any(out, std::get<any>(child.value), element.uri);
  }
  put(out, "</");
  put(out, element.local);
  *out++ = '>';
}

template <class T, class Out>
constexpr void write_element(Out& out, const T& value, std::string_view uri, std::string_view local,
                             std::string_view in_effect);

// One occurrence of a child member.
template <class One, members::kind What, class Out>
constexpr void write_child(Out& out, const One& value, std::string_view uri, std::string_view local,
                           std::string_view in_effect) {
  if constexpr (What == members::kind::child) {
    write_element(out, value, uri, local, in_effect);
  } else {
    open(out, uri, local, in_effect, {});
    *out++ = '>';
    put_text(out, text_of(value));
    put(out, "</");
    put(out, local);
    *out++ = '>';
  }
}

template <class T, class Out>
constexpr void write_element(Out& out, const T& value, std::string_view uri, std::string_view local,
                             std::string_view in_effect) {
  using info = described_member<T>;
  constexpr std::size_t count = info::schema.count;
  std::vector<std::tuple<std::string_view, std::string_view, std::string>> attributes;
  bool has_content = false;
  [&]<std::size_t... I>(std::index_sequence<I...>) {
    (info::template check<I>(), ...);
    const auto one = [&]<std::size_t K>(std::integral_constant<std::size_t, K>) {
      constexpr members::kind what = info::template kind_of<K>();
      const auto& member = boost::pfr::get<K>(value);
      using held = std::remove_cvref_t<decltype(member)>;
      if constexpr (what == members::kind::attribute) {
        const std::string_view attribute_uri = info::schema.bound[K].uri.value_or(std::string_view());
        if constexpr (is_optional<held>::value) {
          if (member)
            attributes.emplace_back(attribute_uri, info::template local_of<K>(), text_of(*member));
        } else {
          attributes.emplace_back(attribute_uri, info::template local_of<K>(), text_of(member));
        }
      } else if constexpr (is_optional<held>::value) {
        has_content = has_content || member.has_value();
      } else if constexpr (is_vector<held>::value) {
        has_content = has_content || !member.empty();
      } else if constexpr (what == members::kind::text) {
        has_content = has_content || !text_of(member).empty();
      } else {
        has_content = true;
      }
    };
    (one(std::integral_constant<std::size_t, I>{}), ...);
  }(std::make_index_sequence<count>{});

  open(out, uri, local, in_effect, attributes);
  if (!has_content) {
    put(out, "/>");
    return;
  }
  *out++ = '>';
  [&]<std::size_t... I>(std::index_sequence<I...>) {
    const auto one = [&]<std::size_t K>(std::integral_constant<std::size_t, K>) {
      constexpr members::kind what = info::template kind_of<K>();
      const auto& member = boost::pfr::get<K>(value);
      using held = std::remove_cvref_t<decltype(member)>;
      using one_value = element_of<held>;
      if constexpr (what == members::kind::text) {
        if constexpr (is_optional<held>::value) {
          if (member)
            put_text(out, text_of(*member));
        } else {
          put_text(out, text_of(member));
        }
      } else if constexpr (what == members::kind::unknown_children) {
        for (const any& kept : member)
          write_any(out, kept, uri);
      } else if constexpr (what == members::kind::child || what == members::kind::child_text) {
        constexpr auto name = info::template child_name<K>();
        const std::string_view child_uri = name.second.value_or(uri);
        const std::string_view child_local = name.first;
        if constexpr (is_optional<held>::value) {
          if (member)
            write_child<one_value, what>(out, *member, child_uri, child_local, uri);
        } else if constexpr (is_vector<held>::value) {
          for (const auto& each : member)
            write_child<one_value, what>(out, each, child_uri, child_local, uri);
        } else {
          write_child<one_value, what>(out, member, child_uri, child_local, uri);
        }
      }
    };
    (one(std::integral_constant<std::size_t, I>{}), ...);
  }(std::make_index_sequence<count>{});
  put(out, "</");
  put(out, local);
  *out++ = '>';
}

}  // namespace chevron::detail::writing

export namespace chevron {

// A value written as XML by its schema, to an output iterator of char: the
// element with a default namespace declaration where it has a namespace,
// attributes, then its members in order. Where it ends is returned.
template <described T, std::output_iterator<char> Out>
constexpr Out write(Out out, const T& value) {
  constexpr auto schema = xml_schema(type<T>{});
  static_assert(schema.named, "chevron: a type written on its own needs .name() in its schema");
  detail::writing::write_element(out, value, schema.uri, schema.local, std::string_view());
  return out;
}


}  // namespace chevron

namespace chevron::detail::lazy {

using reading::element_of;
using reading::is_optional;
using reading::is_vector;
using reading::described_member;
using writing::put;
using writing::put_text;
using writing::text_of;

// One element being written, as a machine that gives the next piece of text
// each time it is asked -- or an element nested in it to write first, or that
// it is done. The pieces stay valid until it is asked again.
struct frame {
  struct step {
    enum class what : std::uint8_t { piece, child, done } kind = what::done;
    std::string_view text{};
    std::unique_ptr<frame> child{};
  };
  virtual ~frame() = default;
  virtual step next() = 0;
};

using step = frame::step;

inline step piece(std::string_view text) { return {step::what::piece, text, nullptr}; }
inline step child(std::unique_ptr<frame> inner) { return {step::what::child, {}, std::move(inner)}; }
inline step done() { return {}; }

std::string start_tag(std::string_view uri, std::string_view local, std::string_view in_effect,
                      const std::vector<std::tuple<std::string_view, std::string_view, std::string>>& attributes,
                      bool empty) {
  std::string out;
  auto at = std::back_inserter(out);
  writing::open(at, uri, local, in_effect, attributes);
  put(at, empty ? "/>" : ">");
  return out;
}

class any_frame final : public frame {
 public:
  any_frame(const any& element, std::string_view in_effect) : element_(element), in_effect_(in_effect) {}

  step next() override {
    if (phase_ == 0) {
      std::vector<std::tuple<std::string_view, std::string_view, std::string>> attributes;
      for (const auto& [name, value] : element_.attributes)
        attributes.emplace_back(name.first, name.second, value);
      buffer_ = start_tag(element_.uri, element_.local, in_effect_, attributes, element_.children.empty());
      phase_ = element_.children.empty() ? 2 : 1;
      return piece(buffer_);
    }
    if (phase_ == 1) {
      if (at_ < element_.children.size()) {
        const any_node& one = element_.children[at_++];
        if (const auto* text = std::get_if<std::string>(&one.value)) {
          buffer_.clear();
          auto out = std::back_inserter(buffer_);
          put_text(out, *text);
          return piece(buffer_);
        }
        return child(std::make_unique<any_frame>(std::get<any>(one.value), element_.uri));
      }
      buffer_ = "</" + element_.local + ">";
      phase_ = 2;
      return piece(buffer_);
    }
    return done();
  }

 private:
  const any& element_;
  std::string_view in_effect_;
  int phase_ = 0;
  std::size_t at_ = 0;
  std::string buffer_;
};

template <class T>
class element_frame final : public frame {
  using info = described_member<T>;
  static constexpr std::size_t count = info::schema.count;

 public:
  element_frame(const T& value, std::string_view uri, std::string_view local, std::string_view in_effect)
      : value_(value), uri_(uri), local_(local), in_effect_(in_effect) {}

  step next() override {
    if (phase_ == 0)
      return open();
    if (phase_ == 1) {
      while (member_ < count) {
        std::optional<step> out = member(std::make_index_sequence<count>{});
        if (out)
          return std::move(*out);
      }
      buffer_ = "</" + std::string(local_) + ">";
      phase_ = 2;
      return piece(buffer_);
    }
    return done();
  }

 private:
  step open() {
    std::vector<std::tuple<std::string_view, std::string_view, std::string>> attributes;
    bool content = false;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (info::template check<I>(), ...);
      const auto one = [&]<std::size_t K>(std::integral_constant<std::size_t, K>) {
        constexpr members::kind what = info::template kind_of<K>();
        const auto& held = boost::pfr::get<K>(value_);
        using type = std::remove_cvref_t<decltype(held)>;
        if constexpr (what == members::kind::attribute) {
          const std::string_view in = info::schema.bound[K].uri.value_or(std::string_view());
          if constexpr (is_optional<type>::value) {
            if (held)
              attributes.emplace_back(in, info::template local_of<K>(), text_of(*held));
          } else {
            attributes.emplace_back(in, info::template local_of<K>(), text_of(held));
          }
        } else if constexpr (is_optional<type>::value) {
          content = content || held.has_value();
        } else if constexpr (is_vector<type>::value) {
          content = content || !held.empty();
        } else if constexpr (what == members::kind::text) {
          content = content || !text_of(held).empty();
        } else {
          content = true;
        }
      };
      (one(std::integral_constant<std::size_t, I>{}), ...);
    }(std::make_index_sequence<count>{});
    buffer_ = start_tag(uri_, local_, in_effect_, attributes, !content);
    phase_ = content ? 1 : 2;
    return piece(buffer_);
  }

  // The next thing member_ gives, if it gives any more; otherwise on to the
  // member after it.
  template <std::size_t... I>
  std::optional<step> member(std::index_sequence<I...>) {
    std::optional<step> out;
    ((member_ == I ? (out = member_at<I>(), true) : false) || ...);
    return out;
  }

  template <std::size_t K>
  std::optional<step> member_at() {
    constexpr members::kind what = info::template kind_of<K>();
    const auto& held = boost::pfr::get<K>(value_);
    using type = std::remove_cvref_t<decltype(held)>;
    using one_value = element_of<type>;
    const auto move_on = [&] {
      ++member_;
      index_ = 0;
    };
    if constexpr (what == members::kind::attribute) {
      move_on();
      return std::nullopt;
    } else if constexpr (what == members::kind::text) {
      move_on();
      buffer_.clear();
      auto out = std::back_inserter(buffer_);
      if constexpr (is_optional<type>::value) {
        if (!held)
          return std::nullopt;
        put_text(out, text_of(*held));
      } else {
        put_text(out, text_of(held));
      }
      return piece(buffer_);
    } else if constexpr (what == members::kind::unknown_children) {
      if (index_ >= held.size()) {
        move_on();
        return std::nullopt;
      }
      return child(std::make_unique<any_frame>(held[index_++], uri_));
    } else {
      // An occurrence of a child: the index-th of a vector, or the one there
      // is of an optional or a plain member.
      const one_value* occurrence = nullptr;
      if constexpr (is_vector<type>::value) {
        if (index_ < held.size())
          occurrence = &held[index_];
      } else if constexpr (is_optional<type>::value) {
        if (index_ == 0 && held)
          occurrence = &*held;
      } else {
        if (index_ == 0)
          occurrence = &held;
      }
      if (!occurrence) {
        move_on();
        return std::nullopt;
      }
      ++index_;
      constexpr auto name = info::template child_name<K>();
      const std::string_view child_uri = name.second.value_or(uri_);
      if constexpr (what == members::kind::child) {
        return child(std::make_unique<element_frame<one_value>>(*occurrence, child_uri, name.first, uri_));
      } else {
        buffer_ = start_tag(child_uri, name.first, uri_, {}, false);
        auto out = std::back_inserter(buffer_);
        put_text(out, text_of(*occurrence));
        put(out, "</");
        put(out, name.first);
        put(out, ">");
        return piece(buffer_);
      }
    }
  }

  const T& value_;
  std::string_view uri_;
  std::string_view local_;
  std::string_view in_effect_;
  int phase_ = 0;
  std::size_t member_ = 0;
  std::size_t index_ = 0;
  std::string buffer_;
};

// The writing machine: a stack of frames, and the piece being given out.
class machine {
 public:
  machine() = default;
  explicit machine(std::unique_ptr<frame> root) { stack_.push_back(std::move(root)); }

  // The next piece, or nothing where the document is written.
  std::optional<std::string_view> next() {
    while (!stack_.empty()) {
      step one = stack_.back()->next();
      switch (one.kind) {
        case step::what::piece:
          if (!one.text.empty())
            return one.text;
          break;
        case step::what::child:
          stack_.push_back(std::move(one.child));
          break;
        case step::what::done:
          stack_.pop_back();
          break;
      }
    }
    return std::nullopt;
  }

 private:
  std::vector<std::unique_ptr<frame>> stack_;
};

}  // namespace chevron::detail::lazy

export namespace chevron {

// A value as XML, lazily: a view of its characters, made as they are pulled --
// piece by piece, with nothing of the document held but the piece being
// read. to_xml(value) | std::ranges::to<std::string>() for a string;
// .chunks() for the pieces themselves, which is cheaper to copy from. A value
// given as an rvalue is kept by the view; one given as an lvalue is referred
// to, and has to outlive it.
template <described T>
class xml_view : public std::ranges::view_interface<xml_view<T>> {
 public:
  class iterator {
   public:
    using value_type = char;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;

    iterator() = default;
    explicit iterator(xml_view* view) : view_(view) { view_->pull(); }
    iterator(iterator&&) = default;
    iterator& operator=(iterator&&) = default;

    char operator*() const { return view_->piece_[view_->at_]; }
    iterator& operator++() {
      if (++view_->at_ == view_->piece_.size())
        view_->pull();
      return *this;
    }
    void operator++(int) { ++*this; }
    friend bool operator==(const iterator& one, std::default_sentinel_t) { return one.view_->done_; }

   private:
    xml_view* view_ = nullptr;
  };

  // The pieces the characters come in.
  class chunk_view : public std::ranges::view_interface<chunk_view> {
   public:
    class iterator {
     public:
      using value_type = std::string_view;
      using difference_type = std::ptrdiff_t;
      using iterator_concept = std::input_iterator_tag;

      iterator() = default;
      explicit iterator(xml_view* view) : view_(view) { view_->pull(); }
      iterator(iterator&&) = default;
      iterator& operator=(iterator&&) = default;

      std::string_view operator*() const { return view_->piece_; }
      iterator& operator++() {
        view_->pull();
        return *this;
      }
      void operator++(int) { ++*this; }
      friend bool operator==(const iterator& one, std::default_sentinel_t) { return one.view_->done_; }

     private:
      xml_view* view_ = nullptr;
    };

    explicit chunk_view(xml_view* view) : view_(view) {}
    iterator begin() {
      view_->start();
      return iterator(view_);
    }
    std::default_sentinel_t end() const noexcept { return {}; }

   private:
    xml_view* view_;
  };

  explicit xml_view(const T& value) : value_(&value) {}
  explicit xml_view(T&& value) : owned_(std::make_unique<T>(std::move(value))), value_(owned_.get()) {}
  xml_view(xml_view&&) = default;
  xml_view& operator=(xml_view&&) = default;

  iterator begin() {
    start();
    return iterator(this);
  }
  std::default_sentinel_t end() const noexcept { return {}; }

  // The same document, as the pieces it is made in.
  chunk_view chunks() { return chunk_view(this); }

 private:
  void start() {
    constexpr auto schema = xml_schema(type<T>{});
    static_assert(schema.named, "chevron: a type written on its own needs .name() in its schema");
    machine_ = detail::lazy::machine(std::make_unique<detail::lazy::element_frame<T>>(
        *value_, schema.uri, schema.local, std::string_view()));
    done_ = false;
  }
  void pull() {
    at_ = 0;
    if (const auto next = machine_.next())
      piece_ = *next;
    else
      done_ = true;
  }

  std::unique_ptr<T> owned_;
  const T* value_ = nullptr;
  detail::lazy::machine machine_;
  std::string_view piece_;
  std::size_t at_ = 0;
  bool done_ = true;
};

template <class T>
  requires described<std::remove_cvref_t<T>>
xml_view<std::remove_cvref_t<T>> to_xml(T&& value) {
  return xml_view<std::remove_cvref_t<T>>(std::forward<T>(value));
}

}  // namespace chevron
