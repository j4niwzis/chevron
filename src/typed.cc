// SPDX-License-Identifier: AGPL-3.0-only
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
import splice;
import boost.pfr;
import chevron.parser;
import chevron.escape;

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

// Attributes kept as they came: {namespace URI, local name}, value.
using kept_attributes = std::vector<std::pair<std::pair<std::string, std::string>, std::string>>;

// A subtree kept as it came, for what a schema does not describe.
struct any_node;
struct any {
  std::string uri;
  std::string local;
  std::vector<std::pair<std::pair<std::string, std::string>, std::string>> attributes;  // {uri, local}, value
  std::vector<any_node> children;
};
struct any_node {
  spl::variant<any, std::string> value;  // an element, or text
};

// One of several element types, chosen by the element's name -- the one each
// alternative's schema gives, with its .when<>() where it has one -- and read
// straight into that type: no tree on the way. chevron::any, last, takes an
// element no alternative names; it is the only tree there is.
//   std::vector<chevron::tagged<delay, chat_state, chevron::any>> extensions;
template <class... Alternatives>
  requires(sizeof...(Alternatives) > 0)
class tagged {
  static_assert(
      [] {
        constexpr bool kept[] = {std::same_as<Alternatives, any>...};
        for (std::size_t at = 0; at + 1 < sizeof...(Alternatives); ++at)
          if (kept[at])
            return false;
        return true;
      }(),
      "chevron: chevron::any may only be the last alternative of a tagged");

 public:
  // Whether T is one of the alternatives.
  template <class T>
  static constexpr bool can_hold = (std::same_as<T, Alternatives> || ...);

  constexpr tagged() = default;
  template <class T>
    requires(std::same_as<std::remove_cvref_t<T>, Alternatives> || ...)
  constexpr tagged(T&& value) : data_(std::forward<T>(value)) {}

  template <class T>
    requires can_hold<T>
  constexpr bool is() const noexcept { return spl::holds_alternative<T>(data_); }
  template <class T>
    requires can_hold<T>
  constexpr T& as() { return spl::get<T>(data_); }
  template <class T>
    requires can_hold<T>
  constexpr const T& as() const { return spl::get<T>(data_); }
  template <class T>
    requires can_hold<T>
  constexpr T* get_if() noexcept { return spl::get_if<T>(&data_); }
  template <class T>
    requires can_hold<T>
  constexpr const T* get_if() const noexcept { return spl::get_if<T>(&data_); }
  constexpr spl::variant<Alternatives...>& data() noexcept { return data_; }
  constexpr const spl::variant<Alternatives...>& data() const noexcept { return data_; }

  // f called with the alternative held: a branch for each, which the
  // optimizer can see through -- no table of functions called indirectly,
  // as std::visit makes.
  template <class F>
  constexpr void with(F&& f) const {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((data_.index() == I ? (void)f(*spl::get_if<I>(&data_)) : void()), ...);
    }(std::index_sequence_for<Alternatives...>{});
  }
  template <class F>
  constexpr void with(F&& f) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((data_.index() == I ? (void)f(*spl::get_if<I>(&data_)) : void()), ...);
    }(std::index_sequence_for<Alternatives...>{});
  }

 private:
  spl::variant<Alternatives...> data_;
};

namespace members {

enum class kind : std::uint8_t {
  deduced, attribute, child_text, child, text, unknown_children, unknown_attributes,
  tagged  // a chevron::tagged: a child chosen by its name
};

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
// Every attribute nothing else claims, kept as it came: chevron::kept_attributes.
constexpr descriptor unknown_attributes() { return {kind::unknown_attributes}; }
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
  // An attribute with a value of its own, which says this type among several
  // of one name: <message type="chat"> is one type, <message type="groupchat">
  // another; absent means this one where or_absent says so.
  bool has_when = false;
  std::string_view when_local{};
  std::string_view when_value{};
  bool when_absent = false;
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

  // The attribute that tells this type apart, and its value.
  template <fixed_string Attribute>
  constexpr schema_t when(std::string_view value, bool or_absent = false) const {
    schema_t out = *this;
    out.has_when = true;
    out.when_local = Attribute.view();
    out.when_value = value;
    out.when_absent = or_absent;
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

// For .when(): the attribute may be absent, and then means this type.
inline constexpr bool or_absent = true;

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

template <class T> struct is_tagged : std::false_type {};
template <class... A> struct is_tagged<tagged<A...>> : std::true_type {};

// A choice among empty types, each naming the text it stands for:
//   struct chat { static constexpr std::string_view xml_value = "chat"; };
//   spl::variant<normal, chat, groupchat, headline, error> type;
template <class T>
struct is_choice : std::false_type {};
template <class... Alternatives>
  requires(requires { std::string_view(Alternatives::xml_value); } && ...)
struct is_choice<spl::variant<Alternatives...>> : std::true_type {};

template <class T>
concept text_like = std::same_as<T, std::string> || std::same_as<T, bool> ||
                    (std::is_arithmetic_v<T> && !std::same_as<T, char>) || is_choice<T>::value;

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
  } else if constexpr (is_choice<T>::value) {
    // The alternative that names this text.
    std::optional<T> out;
    [&]<std::size_t... At>(std::index_sequence<At...>) {
      (void)((text == spl::variant_alternative_t<At, T>::xml_value
                  ? (out.emplace(std::in_place_index<At>), true)
                  : false) ||
             ...);
    }(std::make_index_sequence<spl::variant_size_v<T>>{});
    return out;
  } else {
    T out{};
    const auto [end, problem] = std::from_chars(text.data(), text.data() + text.size(), out);
    if (problem != std::errc{} || end != text.data() + text.size())
      return std::nullopt;
    return out;
  }
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
    if (const auto* child = spl::get_if<start_element>(&*next)) {
      auto inner = capture(source, *child);
      if (!inner)
        return std::unexpected(inner.error());
      out.children.push_back({std::move(*inner)});
    } else if (const auto* piece = spl::get_if<text>(&*next)) {
      out.children.push_back({std::string(piece->content)});
    } else {
      return out;
    }
  }
}

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
    } else if constexpr (is_tagged<element_of<member_type<I>>>::value) {
      return members::kind::tagged;
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
    else if constexpr (what == members::kind::unknown_attributes)
      static_assert(std::same_as<held, kept_attributes>,
                    "chevron: unknown_attributes() needs a chevron::kept_attributes");
  }
};

template <class T>
constexpr bool is_named(const start_element& start) {
  constexpr auto schema = xml_schema(type<T>{});
  if (schema.named && (start.name.uri != schema.uri || start.name.local != schema.local))
    return false;
  if constexpr (schema.has_when) {
    for (const attribute& one : start.attributes)
      if (one.name.uri.empty() && one.name.local == schema.when_local)
        return one.value == schema.when_value;
    return schema.when_absent;
  }
  return true;
}

}  // namespace chevron::detail::reading

// Reusable positions, with shallow nesting held inline. Positions contain
// progress, not erased writers/readers or pointers into the value being built.
namespace chevron::detail {
template <class Position>
class positions {
 public:
  constexpr Position& operator[](std::size_t level) {
    return level < local_.size() ? local_[level] : more_[level - local_.size()];
  }
  constexpr void ensure(std::size_t level) {
    if (level >= local_.size() && more_.size() <= level - local_.size())
      more_.resize(level - local_.size() + 1);
    while (size_ <= level) (*this)[size_++].reset();
  }
  constexpr void trim(std::size_t size) { size_ = size; }
 private:
  std::array<Position, 8> local_{};
  std::vector<Position> more_;
  std::size_t size_ = 0;
};
}  // namespace chevron::detail

namespace chevron::detail::reading {

struct read_position {
  std::string uri;
  std::string text;
  std::uint64_t seen = 0;
  std::vector<std::uint64_t> more_seen;
  std::size_t member = std::variant_npos;
  std::size_t index = 0;
  std::size_t skipping = 0;
  bool text_seen = false;

  constexpr void reset() {
    uri.clear();
    text.clear();
    seen = 0;
    std::ranges::fill(more_seen, 0);
    member = std::variant_npos;
    index = skipping = 0;
    text_seen = false;
  }
  constexpr void mark(std::size_t at) {
    if (at < 64) seen |= std::uint64_t{1} << at;
    else {
      if (more_seen.size() < at / 64) more_seen.resize(at / 64);
      more_seen[at / 64 - 1] |= std::uint64_t{1} << (at % 64);
    }
  }
  constexpr bool has(std::size_t at) const {
    return at < 64 ? (seen & (std::uint64_t{1} << at)) != 0
                  : at / 64 <= more_seen.size() &&
                    (more_seen[at / 64 - 1] & (std::uint64_t{1} << (at % 64))) != 0;
  }
};

// Call f with the selected concrete type. Unknown elements reach any only
// where the tagged explicitly includes it.
template <class Tagged, class F>
constexpr bool select(const start_element& start, F&& f) {
  bool matched = false;
  [&]<class... A>(type<tagged<A...>>) {
    ([&] {
      if (matched) return;
      if constexpr (!std::same_as<A, any>) {
        static_assert(described<A> && xml_schema(type<A>{}).named,
                      "chevron: a tagged alternative needs a named schema");
        if (!is_named<A>(start)) return;
      }
      matched = true;
      f(type<A>{});
    }(), ...);
  }(type<Tagged>{});
  return matched;
}

class incremental {
  using result = std::expected<bool, read_error>;
 public:
  constexpr void reset() { at_.trim(0); }

  template <class T>
  constexpr std::expected<void, read_error> start(T& out, const start_element& start,
                                                 std::size_t level = 0) {
    at_.ensure(level);
    if constexpr (std::same_as<T, any>) {
      out.uri = start.name.uri;
      out.local = start.name.local;
      for (const auto& attribute : start.attributes)
        out.attributes.push_back({{std::string(attribute.name.uri), std::string(attribute.name.local)},
                                  std::string(attribute.value)});
      return {};
    } else {
      using info = described_member<T>;
      at_[level].uri = start.name.uri;
      std::optional<read_error> failure;
      [&]<std::size_t... I>(std::index_sequence<I...>) {
        (info::template check<I>(), ...);
        ([&] {
          if constexpr (info::template kind_of<I>() == members::kind::attribute) {
            if (failure) return;
            const auto local = info::template local_of<I>();
            const auto uri = info::schema.bound[I].uri.value_or(std::string_view());
            auto& member = boost::pfr::get<I>(out);
            using held = std::remove_cvref_t<decltype(member)>;
            for (const attribute& found : start.attributes) {
              if (found.name.local != local || found.name.uri != uri) continue;
              auto value = value_of<element_of<held>>(found.value);
              if (!value) failure = read_error{read_code::bad_value, std::string(local), {}};
              else member = std::move(*value);
              return;
            }
            if constexpr (!is_optional<held>::value)
              failure = read_error{read_code::missing_attribute, std::string(local), {}};
          }
        }(), ...);
        ([&] {
          if constexpr (info::template kind_of<I>() == members::kind::unknown_attributes) {
            for (const attribute& found : start.attributes) {
              const bool claimed = [&]<std::size_t... K>(std::index_sequence<K...>) {
                return (false || ... || [&] {
                  if constexpr (info::template kind_of<K>() == members::kind::attribute)
                    return found.name.local == info::template local_of<K>() &&
                           found.name.uri == info::schema.bound[K].uri.value_or(std::string_view());
                  else return false;
                }());
              }(std::make_index_sequence<info::schema.count>{});
              if (!claimed) boost::pfr::get<I>(out).push_back(
                  {{std::string(found.name.uri), std::string(found.name.local)}, std::string(found.value)});
            }
          }
        }(), ...);
      }(std::make_index_sequence<info::schema.count>{});
      if (failure) return std::unexpected(std::move(*failure));
      return {};
    }
  }

  template <class T>
  constexpr result consume(T& out, const event& event, std::size_t level = 0) {
    if constexpr (std::same_as<T, any>) return capture(out, event, level);
    else {
      using info = described_member<T>;
      constexpr auto count = info::schema.count;
      if (at_[level].skipping) {
        if (spl::holds_alternative<start_element>(event)) ++at_[level].skipping;
        else if (spl::holds_alternative<end_element>(event)) --at_[level].skipping;
        return false;
      }
      if (at_[level].member != std::variant_npos) {
        result done = false;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
          (void)((at_[level].member == I ? (done = child<T, I>(out, event, level), true) : false) || ...);
        }(std::make_index_sequence<count>{});
        if (!done) return done;
        if (*done) {
          at_[level].member = std::variant_npos;
          at_.trim(level + 1);
        }
        return false;
      }
      if (const auto* piece = spl::get_if<text>(&event)) {
        bool needs_text = false;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
          ([&] {
            if constexpr (info::template kind_of<I>() == members::kind::text) {
              auto& member = boost::pfr::get<I>(out);
              using held = std::remove_cvref_t<decltype(member)>;
              if constexpr (std::same_as<element_of<held>, std::string>) {
                if constexpr (is_optional<held>::value) {
                  if (!member) member.emplace();
                  *member += piece->content;
                } else member += piece->content;
              } else needs_text = true;
            }
          }(), ...);
        }(std::make_index_sequence<count>{});
        if (needs_text) {
          at_[level].text += piece->content;
          at_[level].text_seen = true;
        }
        return false;
      }
      if (spl::holds_alternative<end_element>(event)) {
        std::optional<read_error> failure;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
          ([&] {
            if (failure) return;
            constexpr auto kind = info::template kind_of<I>();
            using held = typename info::template member_type<I>;
            if constexpr (kind == members::kind::text &&
                          !std::same_as<element_of<held>, std::string>) {
              if (at_[level].text_seen) {
                auto value = value_of<element_of<held>>(at_[level].text);
                if (!value) failure = read_error{read_code::bad_value, "text", {}};
                else boost::pfr::get<I>(out) = std::move(*value);
              }
            }
            if constexpr ((kind == members::kind::child || kind == members::kind::child_text ||
                           kind == members::kind::tagged) &&
                          !is_optional<held>::value && !is_vector<held>::value) {
              if (!at_[level].has(I)) failure = read_error{
                  read_code::missing_child, std::string(info::template child_name<I>().first), {}};
            }
          }(), ...);
        }(std::make_index_sequence<count>{});
        if (failure) return std::unexpected(std::move(*failure));
        return true;
      }
      const auto& incoming = spl::get<start_element>(event);
      bool claimed = false;
      std::expected<void, read_error> began;
      [&]<std::size_t... I>(std::index_sequence<I...>) {
        ([&] {
          if (claimed) return;
          constexpr auto kind = info::template kind_of<I>();
          auto& member = boost::pfr::get<I>(out);
          using held = std::remove_cvref_t<decltype(member)>;
          if constexpr (kind == members::kind::child || kind == members::kind::child_text) {
            constexpr auto name = info::template child_name<I>();
            if (incoming.name.local != name.first ||
                incoming.name.uri != name.second.value_or(at_[level].uri)) return;
            claimed = true;
            auto& value = begin<I>(member, level);
            if constexpr (kind == members::kind::child) began = start(value, incoming, level + 1);
          } else if constexpr (kind == members::kind::tagged) {
            claimed = select<element_of<held>>(incoming, [&]<class A>(type<A>) {
              auto& value = begin<I>(member, level);
              auto& alternative = value.data().template emplace<A>();
              began = start(alternative, incoming, level + 1);
            });
          }
        }(), ...);
        if (claimed) return;
        ([&] {
          if constexpr (info::template kind_of<I>() == members::kind::unknown_children) {
            if (claimed) return;
            claimed = true;
            auto& value = begin<I>(boost::pfr::get<I>(out), level);
            began = start(value, incoming, level + 1);
          }
        }(), ...);
      }(std::make_index_sequence<count>{});
      if (!began) return std::unexpected(std::move(began.error()));
      if (!claimed) at_[level].skipping = 1;
      return false;
    }
  }

 private:
  template <std::size_t I, class Member>
  constexpr auto& begin(Member& member, std::size_t level) {
    at_[level].member = I;
    at_[level].mark(I);
    at_.ensure(level + 1);
    if constexpr (is_vector<Member>::value) {
      at_[level].index = member.size();
      return member.emplace_back();
    } else if constexpr (is_optional<Member>::value) return member.emplace();
    else { member = {}; return member; }
  }

  template <class T, std::size_t I>
  constexpr result child(T& out, const event& event, std::size_t level) {
    using info = described_member<T>;
    constexpr auto kind = info::template kind_of<I>();
    if constexpr (kind == members::kind::attribute || kind == members::kind::unknown_attributes ||
                  kind == members::kind::text) return false;
    else {
      auto& member = boost::pfr::get<I>(out);
      auto& value = [&]() -> auto& {
        using held = std::remove_cvref_t<decltype(member)>;
        if constexpr (is_vector<held>::value) return member[at_[level].index];
        else if constexpr (is_optional<held>::value) return *member;
        else return member;
      }();
      if constexpr (kind == members::kind::child_text) {
        if (const auto* text = spl::get_if<chevron::text>(&event)) {
          if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, std::string>) value += text->content;
          else at_[level + 1].text += text->content;
          return false;
        }
        const auto name = info::template child_name<I>().first;
        if (spl::holds_alternative<start_element>(event))
          return std::unexpected(read_error{read_code::bad_value, std::string(name), {}});
        if constexpr (!std::same_as<std::remove_cvref_t<decltype(value)>, std::string>) {
          auto parsed = value_of<std::remove_cvref_t<decltype(value)>>(at_[level + 1].text);
          if (!parsed) return std::unexpected(read_error{read_code::bad_value, std::string(name), {}});
          value = std::move(*parsed);
        }
        return true;
      } else if constexpr (kind == members::kind::tagged) {
        result done = false;
        value.with([&](auto& alternative) { done = consume(alternative, event, level + 1); });
        return done;
      } else return consume(value, event, level + 1);
    }
  }

  constexpr result capture(any& out, const event& event, std::size_t level) {
    if (at_[level].member != std::variant_npos) {
      auto done = capture(spl::get<any>(out.children[at_[level].member].value), event, level + 1);
      if (done && *done) {
        at_[level].member = std::variant_npos;
        at_.trim(level + 1);
      }
      return done ? result(false) : done;
    }
    if (const auto* text = spl::get_if<chevron::text>(&event)) {
      out.children.push_back({std::string(text->content)});
      return false;
    }
    if (const auto* child = spl::get_if<start_element>(&event)) {
      at_[level].member = out.children.size();
      out.children.push_back({any{}});
      auto begun = start(spl::get<any>(out.children.back().value), *child, level + 1);
      return begun ? result(false) : std::unexpected(std::move(begun.error()));
    }
    return true;
  }

  positions<read_position> at_;
};

// The synchronous API uses the same state machine, completing an element
// before returning. reader<T> below keeps this state between calls instead.
template <class T, class Source>
constexpr std::expected<T, read_error> read_element(Source& source, const start_element& first) {
  T out{};
  incremental state;
  auto started = state.start(out, first);
  if (!started) return std::unexpected(std::move(started.error()));
  for (;;) {
    auto next = next_event(source);
    if (!next) return std::unexpected(std::move(next.error()));
    auto done = state.consume(out, *next);
    if (!done) return std::unexpected(std::move(done.error()));
    if (*done) return out;
  }
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

// A persistent typed read. next(source) returns no value when more events
// are needed; feeding the source and calling again continues the same value.
// consume(event) is the equivalent for a caller that already pulls events.
template <described... Types>
  requires(sizeof...(Types) > 0)
class reader {
  template <class... T> struct result_type { using type = spl::variant<T...>; };
  template <class T> struct result_type<T> { using type = T; };
 public:
  using value_type = typename result_type<Types...>::type;
  using result = std::expected<std::optional<value_type>, read_error>;

  constexpr result consume(const event& event) {
    if (failure_) return std::unexpected(*failure_);
    if (!value_) {
      if (const auto* text = spl::get_if<chevron::text>(&event);
          text && std::ranges::all_of(text->content, [](char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
          })) return std::nullopt;
      const auto* start = spl::get_if<start_element>(&event);
      if (!start) return fail({read_code::unexpected_element, {}, {}});
      bool matched = false;
      std::expected<void, read_error> begun;
      ([&] {
        if (matched || !detail::reading::is_named<Types>(*start)) return;
        matched = true;
        state_.reset();
        if constexpr (sizeof...(Types) == 1) {
          value_.emplace();
          begun = state_.start(*value_, *start);
        } else {
          value_.emplace(std::in_place_type<Types>);
          begun = state_.start(spl::get<Types>(*value_), *start);
        }
      }(), ...);
      if (!matched) return fail({read_code::unexpected_element, std::string(start->name.local), {}});
      if (!begun) return fail(std::move(begun.error()));
      return std::nullopt;
    }
    auto done = [&] {
      if constexpr (sizeof...(Types) == 1) return state_.consume(*value_, event);
      else return spl::visit([&](auto& held) { return state_.consume(held, event); }, *value_);
    }();
    if (!done) return fail(std::move(done.error()));
    if (!*done) return std::nullopt;
    auto completed = std::move(value_);
    value_.reset();
    state_.reset();
    return completed;
  }

  template <event_source Source>
  constexpr result next(Source& source) {
    if (failure_) return std::unexpected(*failure_);
    for (;;) {
      auto event = source.next();
      if (!event) return fail({read_code::parse, {}, event.error()});
      if (!*event) return std::nullopt;
      auto made = consume(**event);
      if (!made || *made) return made;
    }
  }

  // For event sources without an explicit end/error event. A partial value
  // at the end is an error; ending between values is valid.
  constexpr std::expected<void, read_error> finish() {
    if (failure_) return std::unexpected(*failure_);
    if (value_) {
      failure_ = read_error{read_code::incomplete, {}, {}};
      return std::unexpected(*failure_);
    }
    return {};
  }
  constexpr void reset() {
    value_.reset();
    failure_.reset();
    state_.reset();
  }

 private:
  constexpr result fail(read_error error) {
    failure_ = std::move(error);
    return std::unexpected(*failure_);
  }
  std::optional<value_type> value_;
  detail::reading::incremental state_;
  std::optional<read_error> failure_;
};

// The next event that is not white space between elements.
template <class Source>
constexpr auto next_significant(Source& source) {
  for (;;) {
    auto next = detail::reading::next_event(source);
    if (next) {
      if (const auto* piece = spl::get_if<text>(&*next);
          piece && std::ranges::all_of(piece->content, [](char one) {
            return one == ' ' || one == '\t' || one == '\n' || one == '\r';
          }))
        continue;
    }
    return next;
  }
}

// One element read into a T: the next event must be its start.
template <described T, event_source Source>
constexpr std::expected<T, read_error> read(Source& source) {
  auto next = next_significant(source);
  if (!next)
    return std::unexpected(next.error());
  const auto* start = spl::get_if<start_element>(&*next);
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
constexpr std::expected<spl::variant<T...>, read_error> read_one_of(Source& source) {
  auto next = next_significant(source);
  if (!next)
    return std::unexpected(next.error());
  const auto* start = spl::get_if<start_element>(&*next);
  std::optional<std::expected<spl::variant<T...>, read_error>> out;
  if (start)
    ([&] {
      if (out || !detail::reading::is_named<T>(*start))
        return;
      auto one = detail::reading::read_element<T>(source, *start);
      if (one)
        out.emplace(spl::variant<T...>(std::in_place_type<T>, std::move(*one)));
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
constexpr std::expected<spl::variant<T...>, read_error> read_one_of(Range&& events) {
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

// Character data, and an attribute value in double quotes: escaped as
// chevron.escape says.
template <class Out>
constexpr void put_text(Out& out, std::string_view text) {
  out = std::ranges::copy(escaped_text(text), std::move(out)).out;
}
template <class Out>
constexpr void put_value(Out& out, std::string_view text) {
  out = std::ranges::copy(escaped_value(text), std::move(out)).out;
}

// A borrowed string, or a scalar formatted into inline storage. Copying this
// object never leaves a view pointing into the object it was copied from.
struct scalar_text {
  std::string_view borrowed;
  std::array<char, 64> digits{};
  std::size_t size = 0;
  bool number = false;
  constexpr operator std::string_view() const {
    return number ? std::string_view(digits.data(), size) : borrowed;
  }
  constexpr bool empty() const { return std::string_view(*this).empty(); }
};

template <class T>
constexpr scalar_text text_of(const T& value) {
  scalar_text out;
  if constexpr (std::same_as<T, std::string>) out.borrowed = value;
  else if constexpr (std::same_as<T, bool>) out.borrowed = value ? "true" : "false";
  else if constexpr (reading::is_choice<T>::value)
    out.borrowed = spl::visit([](const auto& one) { return std::string_view(one.xml_value); }, value);
  else {
    const auto [end, problem] = std::to_chars(out.digits.data(), out.digits.data() + out.digits.size(), value);
    out.number = true;
    out.size = end - out.digits.data();
  }
  return out;
}

template <class T, class F>
constexpr void attributes_of(const T& value, F&& emit) {
  using info = described_member<T>;
  [&]<std::size_t... I>(std::index_sequence<I...>) {
    (info::template check<I>(), ...);
    ([&] {
      constexpr auto kind = info::template kind_of<I>();
      const auto& member = boost::pfr::get<I>(value);
      if constexpr (kind == members::kind::attribute) {
        const auto uri = info::schema.bound[I].uri.value_or(std::string_view());
        if constexpr (is_optional<std::remove_cvref_t<decltype(member)>>::value) {
          if (member) emit(uri, info::template local_of<I>(), text_of(*member));
        } else emit(uri, info::template local_of<I>(), text_of(member));
      } else if constexpr (kind == members::kind::unknown_attributes) {
        for (const auto& [name, kept] : member) emit(name.first, name.second, text_of(kept));
      }
    }(), ...);
  }(std::make_index_sequence<info::schema.count>{});
  if constexpr (info::schema.has_when) {
    if (!info::schema.when_absent)
      emit(std::string_view(), info::schema.when_local, scalar_text{info::schema.when_value});
  }
}

constexpr void attributes_of(const any& value, auto&& emit) {
  for (const auto& [name, kept] : value.attributes) emit(name.first, name.second, text_of(kept));
}

// Select just one attribute for the lazy writer. Kept attributes are indexed
// directly, and other values are formatted only when selected.
template <class T, class F>
constexpr bool attribute_at(const T& value, std::size_t index, F&& emit) {
  using info = described_member<T>;
  const bool found = [&]<std::size_t... I>(std::index_sequence<I...>) {
    return (false || ... || [&] {
      constexpr auto kind = info::template kind_of<I>();
      const auto& member = boost::pfr::get<I>(value);
      if constexpr (kind == members::kind::attribute) {
        const auto uri = info::schema.bound[I].uri.value_or(std::string_view());
        if constexpr (is_optional<std::remove_cvref_t<decltype(member)>>::value) {
          if (!member) return false;
          if (index) { --index; return false; }
          emit(uri, info::template local_of<I>(), text_of(*member));
        } else {
          if (index) { --index; return false; }
          emit(uri, info::template local_of<I>(), text_of(member));
        }
        return true;
      } else if constexpr (kind == members::kind::unknown_attributes) {
        if (index >= member.size()) { index -= member.size(); return false; }
        const auto& [name, kept] = member[index];
        emit(name.first, name.second, text_of(kept));
        return true;
      } else return false;
    }());
  }(std::make_index_sequence<info::schema.count>{});
  if (found) return true;
  if constexpr (info::schema.has_when) {
    if (!info::schema.when_absent && index == 0) {
      emit(std::string_view(), info::schema.when_local, scalar_text{info::schema.when_value});
      return true;
    }
  }
  return false;
}
constexpr bool attribute_at(const any& value, std::size_t index, auto&& emit) {
  if (index >= value.attributes.size()) return false;
  const auto& [name, kept] = value.attributes[index];
  emit(name.first, name.second, text_of(kept));
  return true;
}

template <class T>
constexpr bool has_content(const T& value) {
  using info = described_member<T>;
  return [&]<std::size_t... I>(std::index_sequence<I...>) {
    (info::template check<I>(), ...);
    return (false || ... || [&] {
      constexpr auto kind = info::template kind_of<I>();
      const auto& member = boost::pfr::get<I>(value);
      using held = std::remove_cvref_t<decltype(member)>;
      if constexpr (kind == members::kind::attribute || kind == members::kind::unknown_attributes) return false;
      else if constexpr (is_optional<held>::value) return member.has_value();
      else if constexpr (is_vector<held>::value) return !member.empty();
      else if constexpr (kind == members::kind::text) return !text_of(member).empty();
      else return true;
    }());
  }(std::make_index_sequence<info::schema.count>{});
}

inline constexpr std::string_view xml_namespace = "http://www.w3.org/XML/1998/namespace";

template <class Out>
constexpr void open(Out& out, std::string_view uri, std::string_view local, std::string_view in_effect) {
  put(out, "<");
  put(out, local);
  if (uri != in_effect) {
    put(out, " xmlns=\"");
    put_value(out, uri);
    put(out, "\"");
  }
}

template <class Out>
constexpr void write_attribute(Out& out, std::string_view uri, std::string_view local,
                               std::string_view value, std::size_t& prefixes) {
  put(out, " ");
  if (uri == xml_namespace) put(out, "xml:");
  else if (!uri.empty()) {
    const auto prefix = text_of(prefixes++);
    put(out, "xmlns:a");
    put(out, prefix);
    put(out, "=\"");
    put_value(out, uri);
    put(out, "\" a");
    put(out, prefix);
    put(out, ":");
  }
  put(out, local);
  put(out, "=\"");
  put_value(out, value);
  put(out, "\"");
}

template <class Out>
constexpr void write_any(Out& out, const any& element, std::string_view in_effect) {
  open(out, element.uri, element.local, in_effect);
  std::size_t prefixes = 0;
  attributes_of(element, [&](std::string_view uri, std::string_view local, auto text) {
    write_attribute(out, uri, local, text, prefixes);
  });
  if (element.children.empty()) {
    put(out, "/>");
    return;
  }
  *out++ = '>';
  for (const any_node& child : element.children) {
    if (const auto* text = spl::get_if<std::string>(&child.value))
      put_text(out, *text);
    else
      write_any(out, spl::get<any>(child.value), element.uri);
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
    open(out, uri, local, in_effect);
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
  open(out, uri, local, in_effect);
  std::size_t prefixes = 0;
  attributes_of(value, [&](std::string_view attribute_uri, std::string_view attribute_local, auto text) {
    write_attribute(out, attribute_uri, attribute_local, text, prefixes);
  });
  if (!has_content(value)) {
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
      } else if constexpr (what == members::kind::tagged) {
        const auto write_one = [&](const one_value& each) {
          each.with([&]<class One>(const One& alternative) {
            if constexpr (std::same_as<One, any>) {
              write_any(out, alternative, uri);
            } else {
              constexpr auto inner = xml_schema(type<One>{});
              write_element(out, alternative, inner.uri, inner.local, uri);
            }
          });
        };
        if constexpr (is_optional<held>::value) {
          if (member)
            write_one(*member);
        } else if constexpr (is_vector<held>::value) {
          for (const auto& each : member)
            write_one(each);
        } else {
          write_one(member);
        }
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

// An element kept as it came, written as XML.
template <std::output_iterator<char> Out>
constexpr Out write(Out out, const any& kept) {
  detail::writing::write_any(out, kept, std::string_view());
  return out;
}

// An element kept as it came, read as a typed value: written out, and read
// back in by the type's schema.
template <described T>
constexpr std::expected<T, read_error> from_any(const any& kept) {
  std::string text;
  write(std::back_inserter(text), kept);
  parser source;
  source.feed(text);
  return read<T>(source);
}

// A typed value, kept as an element: written out, and read back in whole.
template <described T>
constexpr any to_any(const T& value) {
  std::string text;
  write(std::back_inserter(text), value);
  parser source;
  source.feed(text);
  auto next = detail::reading::next_event(source);
  auto kept = detail::reading::capture(source, spl::get<start_element>(*next));
  return std::move(*kept);
}


}  // namespace chevron

namespace chevron::detail::lazy {

using reading::element_of;
using reading::is_optional;
using reading::is_vector;
using reading::described_member;
using writing::text_of;

enum class escaping { none, text, attribute };
struct piece {
  std::string_view text;
  escaping escape = escaping::none;
};

// Concrete traversal, with an inline position per open element. Large text
// is borrowed and escaped a run at a time; neither tags nor attributes are
// collected into a temporary container.
struct position {
  int phase = 0;
  int header = 0;
  int attribute = 0;
  std::size_t member = 0;
  std::size_t index = 0;
  std::size_t attributes = 0;
  std::size_t prefixes = 0;
  bool content = false;
  std::string_view attribute_uri, attribute_local;
  writing::scalar_text value, prefix;
  constexpr void reset() { *this = {}; }
};

class writer {
 public:
  template <class T>
  constexpr std::optional<std::string_view> next(const T& value, std::string_view uri, std::string_view local) {
    for (;;) {
      if (!pending_.empty()) {
        const auto reference_for = [&](char c) {
          return escape_ == escaping::text ? text_reference(c) : value_reference(c);
        };
        escape_buffer_ = reference_for(pending_.front());
        if (escape_buffer_.size != 1) {
          pending_.remove_prefix(1);
          return std::string_view(escape_buffer_.chars.data(), escape_buffer_.size);
        }
        std::size_t end = 1;
        while (end < pending_.size() && reference_for(pending_[end]).size == 1) ++end;
        const auto run = pending_.substr(0, end);
        pending_.remove_prefix(end);
        return run;
      }
      auto out = element(value, uri, local, {}, 0);
      if (!out) return std::nullopt;
      if (out->text.empty()) continue;
      if (out->escape == escaping::none) return out->text;
      pending_ = out->text;
      escape_ = out->escape;
    }
  }

 private:
  // Emit an attribute in small fragments. Value formatting is inline and
  // remains alive while next() drains any pending escape runs.
  constexpr std::optional<piece> attribute(position& at) {
    const bool xml = at.attribute_uri == writing::xml_namespace;
    for (;;) switch (at.attribute++) {
      case 0: return piece{" "};
      case 1:
        if (xml) { at.attribute = 9; return piece{"xml:"}; }
        if (at.attribute_uri.empty()) { at.attribute = 9; continue; }
        at.prefix = text_of(at.prefixes++);
        return piece{"xmlns:a"};
      case 2: return piece{at.prefix};
      case 3: return piece{"=\""};
      case 4: return piece{at.attribute_uri, escaping::attribute};
      case 5: return piece{"\" a"};
      case 6: return piece{at.prefix};
      case 7: return piece{":"};
      case 8: continue;
      case 9: return piece{at.attribute_local};
      case 10: return piece{"=\""};
      case 11: return piece{at.value, escaping::attribute};
      case 12: return piece{"\""};
      default: return std::nullopt;
    }
  }

  template <class Attributes>
  constexpr std::optional<piece> header(std::string_view uri, std::string_view local,
                                        std::string_view in_effect, std::size_t level, Attributes&& attributes) {
    auto& at = at_[level];
    for (;;) switch (at.header) {
      case 0: ++at.header; return piece{"<"};
      case 1: ++at.header; return piece{local};
      case 2:
        if (uri == in_effect) { at.header = 5; continue; }
        ++at.header; return piece{" xmlns=\""};
      case 3: ++at.header; return piece{uri, escaping::attribute};
      case 4: ++at.header; return piece{"\""};
      case 5: {
        const bool found = attributes(at.attributes, [&](std::string_view uri, std::string_view local,
                                                         writing::scalar_text value) {
          at.attribute_uri = uri;
          at.attribute_local = local;
          at.value = value;
        });
        if (!found) { at.header = 7; continue; }
        ++at.attributes;
        at.attribute = 0;
        at.header = 6;
        continue;
      }
      case 6:
        if (auto out = attribute(at)) return out;
        at.header = 5;
        continue;
      default:
        at.phase = at.content ? 1 : 5;
        return piece{at.content ? ">" : "/>"};
    }
  }

  constexpr std::optional<piece> close(std::string_view local, std::size_t level) {
    switch (at_[level].phase++) {
      case 2: return piece{"</"};
      case 3: return piece{local};
      case 4: return piece{">"};
      default: at_[level].phase = 5; return std::nullopt;
    }
  }

  template <class T>
  constexpr std::optional<piece> element(const T& value, std::string_view uri, std::string_view local,
                                         std::string_view in_effect, std::size_t level) {
    using info = described_member<T>;
    at_.ensure(level);
    if (at_[level].phase == 0) {
      if (at_[level].header == 0) at_[level].content = writing::has_content(value);
      return header(uri, local, in_effect, level, [&](std::size_t index, auto&& emit) { return writing::attribute_at(value, index, emit); });
    }
    if (at_[level].phase == 1) {
      while (at_[level].member < info::schema.count) {
        std::optional<piece> out;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
          (void)((at_[level].member == I ? (out = member<T, I>(value, uri, level), true) : false) || ...);
        }(std::make_index_sequence<info::schema.count>{});
        if (out) return out;
      }
      at_[level].phase = 2;
    }
    return close(local, level);
  }

  constexpr std::optional<piece> kept(const any& value, std::string_view in_effect, std::size_t level) {
    at_.ensure(level);
    if (at_[level].phase == 0) {
      at_[level].content = !value.children.empty();
      return header(value.uri, value.local, in_effect, level,
                    [&](std::size_t index, auto&& emit) { return writing::attribute_at(value, index, emit); });
    }
    if (at_[level].phase == 1) {
      while (at_[level].member < value.children.size()) {
        const auto& child = value.children[at_[level].member];
        if (const auto* text = spl::get_if<std::string>(&child.value)) {
          ++at_[level].member;
          return piece{*text, escaping::text};
        }
        if (auto out = kept(spl::get<any>(child.value), value.uri, level + 1)) return out;
        at_.trim(level + 1);
        ++at_[level].member;
      }
      at_[level].phase = 2;
    }
    return close(value.local, level);
  }

  template <class T>
  constexpr piece text(const T& value) {
    scalar_ = text_of(value);
    return {scalar_, escaping::text};
  }

  template <class T>
  constexpr std::optional<piece> text_child(const T& value, std::string_view uri, std::string_view local,
                                            std::string_view in_effect, std::size_t level) {
    at_.ensure(level);
    if (at_[level].phase == 0) {
      at_[level].content = true;
      return header(uri, local, in_effect, level, [](std::size_t, auto&&) { return false; });
    }
    if (at_[level].phase == 1) {
      at_[level].phase = 2;
      return text(value);
    }
    return close(local, level);
  }

  template <class T, std::size_t K>
  constexpr std::optional<piece> member(const T& value, std::string_view uri, std::size_t level) {
    using info = described_member<T>;
    constexpr auto what = info::template kind_of<K>();
    const auto& held = boost::pfr::get<K>(value);
    using type = std::remove_cvref_t<decltype(held)>;
    using one_value = element_of<type>;
    const auto move_on = [&] { ++at_[level].member; at_[level].index = 0; };
    if constexpr (what == members::kind::attribute || what == members::kind::unknown_attributes) {
      move_on();
      return std::nullopt;
    } else if constexpr (what == members::kind::text) {
      move_on();
      if constexpr (is_optional<type>::value) {
        if (!held) return std::nullopt;
        return text(*held);
      } else return text(held);
    } else {
      const std::size_t index = at_[level].index;
      const one_value* occurrence = nullptr;
      if constexpr (is_vector<type>::value) {
        if (index < held.size()) occurrence = &held[index];
      } else if constexpr (is_optional<type>::value) {
        if (index == 0 && held) occurrence = &*held;
      } else if (index == 0) occurrence = &held;
      if (!occurrence) { move_on(); return std::nullopt; }
      std::optional<piece> out;
      if constexpr (what == members::kind::unknown_children) out = kept(*occurrence, uri, level + 1);
      else if constexpr (what == members::kind::tagged) {
        occurrence->with([&]<class One>(const One& alternative) {
          if constexpr (std::same_as<One, any>) out = kept(alternative, uri, level + 1);
          else {
            constexpr auto inner = xml_schema(chevron::type<One>{});
            out = element(alternative, inner.uri, inner.local, uri, level + 1);
          }
        });
      } else {
        constexpr auto name = info::template child_name<K>();
        const auto child_uri = name.second.value_or(uri);
        if constexpr (what == members::kind::child_text)
          out = text_child(*occurrence, child_uri, name.first, uri, level + 1);
        else out = element(*occurrence, child_uri, name.first, uri, level + 1);
      }
      if (out) return out;
      at_.trim(level + 1);
      ++at_[level].index;
      return std::nullopt;
    }
  }

  positions<position> at_;
  writing::scalar_text scalar_;
  std::string_view pending_;
  escaping escape_ = escaping::none;
  reference escape_buffer_;
};

}  // namespace chevron::detail::lazy

export namespace chevron {

// A value as XML, lazily: a view of its characters, made as they are pulled --
// piece by piece, with nothing of the document held but the piece being
// read and where it is, and nothing erased: std::ranges::to<std::string>(the writer of each element is
// chosen by its type. to_xml(value)) for a string;
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
    constexpr explicit iterator(xml_view* view) : view_(view) { view_->pull(); }
    iterator(iterator&&) = default;
    iterator& operator=(iterator&&) = default;

    constexpr char operator*() const { return view_->piece_[view_->at_]; }
    constexpr iterator& operator++() {
      if (++view_->at_ == view_->piece_.size())
        view_->pull();
      return *this;
    }
    constexpr void operator++(int) { ++*this; }
    friend constexpr bool operator==(const iterator& one, std::default_sentinel_t) { return one.view_->done_; }

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
      constexpr explicit iterator(xml_view* view) : view_(view) { view_->pull(); }
      iterator(iterator&&) = default;
      iterator& operator=(iterator&&) = default;

      constexpr std::string_view operator*() const { return view_->piece_; }
      constexpr iterator& operator++() {
        view_->pull();
        return *this;
      }
      constexpr void operator++(int) { ++*this; }
      friend constexpr bool operator==(const iterator& one, std::default_sentinel_t) { return one.view_->done_; }

     private:
      xml_view* view_ = nullptr;
    };

    constexpr explicit chunk_view(xml_view* view) : view_(view) {}
    constexpr iterator begin() {
      view_->start();
      return iterator(view_);
    }
    constexpr std::default_sentinel_t end() const noexcept { return {}; }

   private:
    xml_view* view_;
  };

  constexpr explicit xml_view(const T& value) : value_(&value) {}
  constexpr explicit xml_view(T&& value) : owned_(std::in_place, std::move(value)), value_(&*owned_) {}
  // Moving invalidates iterators and restarts traversal. Rebind inline-owned
  // values; no piece may keep a pointer into the old view's scalar storage.
  constexpr xml_view(xml_view&& other)
      : owned_(std::move(other.owned_)), value_(owned_ ? &*owned_ : other.value_) {}
  constexpr xml_view& operator=(xml_view&& other) {
    if (this == &other) return *this;
    if (other.owned_) owned_.emplace(std::move(*other.owned_));
    else owned_.reset();
    value_ = owned_ ? &*owned_ : other.value_;
    writer_ = {};
    piece_ = {};
    at_ = 0;
    done_ = true;
    return *this;
  }

  constexpr iterator begin() {
    start();
    return iterator(this);
  }
  constexpr std::default_sentinel_t end() const noexcept { return {}; }

  // The same document, as the pieces it is made in.
  constexpr chunk_view chunks() { return chunk_view(this); }

 private:
  constexpr void start() {
    static_assert(xml_schema(type<T>{}).named, "chevron: a type written on its own needs .name() in its schema");
    writer_ = detail::lazy::writer();
    done_ = false;
  }
  // The next piece that has characters in it.
  constexpr void pull() {
    constexpr auto schema = xml_schema(type<T>{});
    at_ = 0;
    for (;;) {
      const auto next = writer_.next(*value_, schema.uri, schema.local);
      if (!next) {
        done_ = true;
        return;
      }
      if (!next->empty()) {
        piece_ = *next;
        return;
      }
    }
  }

  std::optional<T> owned_;
  const T* value_ = nullptr;
  detail::lazy::writer writer_;
  std::string_view piece_;
  std::size_t at_ = 0;
  bool done_ = true;
};

template <class T>
  requires described<std::remove_cvref_t<T>>
constexpr xml_view<std::remove_cvref_t<T>> to_xml(T&& value) {
  return xml_view<std::remove_cvref_t<T>>(std::forward<T>(value));
}

}  // namespace chevron
