// SPDX-License-Identifier: AGPL-3.0-only
// chevron.escape -- Text made safe inside markup, as references: what
// would end it or start markup written as an entity. Any range of
// characters, lazily -- a text, a view of one, what a reader is handing
// out -- each character escaped to a small range of its own, by value: the
// entity, or the character itself. Nothing is allocated, and nothing points
// into what is escaped; escaped() makes a string, for a caller that keeps
// one.
//
//   std::format(R"(<a href="{:s}">)", chevron::escaped_value(url));
export module chevron.escape;

import std;

export namespace chevron {

// One character, escaped: itself, or its entity -- six characters at most
// ("&quot;"), held by value.
struct reference {
  std::array<char, 6> chars{};
  std::uint8_t size = 0;
  [[nodiscard]] constexpr const char* begin() const { return chars.data(); }
  [[nodiscard]] constexpr const char* end() const { return chars.data() + size; }
  friend constexpr bool operator==(const reference&, const reference&) = default;
};

namespace detail {
[[nodiscard]] constexpr reference as_is(char c) { return {{c}, 1}; }
[[nodiscard]] constexpr reference entity(std::string_view text) {
  reference out{};
  std::ranges::copy(text, out.chars.begin());
  out.size = static_cast<std::uint8_t>(text.size());
  return out;
}
}  // namespace detail

// In character data: what would end it or start markup; and a carriage
// return, which a parser would make a line feed.
[[nodiscard]] constexpr reference text_reference(char c) {
  switch (c) {
    case '&': return detail::entity("&amp;");
    case '<': return detail::entity("&lt;");
    case '>': return detail::entity("&gt;");
    case '\r': return detail::entity("&#xD;");
    default: return detail::as_is(c);
  }
}

// In an attribute value in double quotes -- and so anywhere: the quote, and
// white space other than a space, which attribute-value normalisation would
// otherwise turn into spaces.
[[nodiscard]] constexpr reference value_reference(char c) {
  switch (c) {
    case '&': return detail::entity("&amp;");
    case '<': return detail::entity("&lt;");
    case '"': return detail::entity("&quot;");
    case '\t': return detail::entity("&#x9;");
    case '\n': return detail::entity("&#xA;");
    case '\r': return detail::entity("&#xD;");
    default: return detail::as_is(c);
  }
}

// Characters, as a range of them.
template <class Chars>
concept characters = std::ranges::viewable_range<Chars> && std::ranges::input_range<Chars> &&
                     std::convertible_to<std::ranges::range_reference_t<Chars>, char>;

// A text so, lazily.
template <characters Chars>
[[nodiscard]] constexpr auto escaped_text(Chars&& chars) {
  return std::views::all(std::forward<Chars>(chars)) | std::views::transform([](char c) { return text_reference(c); }) |
         std::views::join;
}
template <characters Chars>
[[nodiscard]] constexpr auto escaped_value(Chars&& chars) {
  return std::views::all(std::forward<Chars>(chars)) | std::views::transform([](char c) { return value_reference(c); }) |
         std::views::join;
}

// The string a caller keeps, or puts together with others: safe in
// character data and in an attribute value alike.
template <characters Chars>
[[nodiscard]] constexpr std::string escaped(Chars&& chars) {
  return escaped_value(std::forward<Chars>(chars)) | std::ranges::to<std::string>();
}

}  // namespace chevron
