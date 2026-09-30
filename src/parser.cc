// SPDX-License-Identifier: AGPL-3.0-only
// The XML of streams, read as events: the subset of XML 1.0 (fifth edition)
// with Namespaces in XML 1.0 that RFC 6120, section 11, allows -- UTF-8 only,
// no comments, processing instructions, document type declarations or entity
// references but the five predefined -- read from bytes as they arrive.
//
// Nothing calls back: the parser is fed bytes, in pieces of any size, and
// asked for the next event, which is a std::variant. What an event refers to
// -- names, attribute values, text -- stays valid until the next call.
export module chevron.parser;

import std;
import splice;

export namespace chevron {

// A name with its namespace resolved: the namespace URI, empty where the name
// is in none, and the local part.
struct qname {
  std::string_view uri;
  std::string_view local;

  friend constexpr bool operator==(const qname&, const qname&) = default;
};

struct attribute {
  qname name;
  std::string_view value;
};

// Namespace declarations -- xmlns and xmlns:prefix -- are not reported as
// attributes: they are what the names of the element and its attributes
// were resolved by.
struct start_element {
  qname name;
  std::span<const attribute> attributes;
};

struct end_element {
  qname name;
};

// Character data, with its references resolved and its line ends made LF;
// the content of a CDATA section is character data too.
struct text {
  std::string_view content;
};

using event = splice::variant<start_element, end_element, text>;

enum class error_code : std::uint8_t {
  ill_formed_utf8,          // bytes that are not UTF-8
  not_a_character,          // a code point XML does not allow (Char)
  unexpected_character,     // something the grammar has no place for here
  bad_name,                 // not a Name, or not a QName
  bad_declaration,          // an XML declaration that is not one
  comment,                  // comments are not allowed
  processing_instruction,   // nor are processing instructions
  document_type,            // nor document type declarations
  bad_reference,            // &...; that is not a reference
  entity,                   // an entity other than the five predefined
  duplicate_attribute,      // the same attribute twice
  unbound_prefix,           // a prefix no xmlns:prefix declares
  bad_namespace,            // a declaration Namespaces in XML forbids
  mismatched_end_tag,       // an end tag for an element not open
  after_root,               // an element after the root has ended
  text_outside_root,        // character data outside the root element
  too_deep,                 // more elements open than the limit
  too_large,                // a token or an element with more than the limit
  unexpected_end,           // the input ended inside the document
};

struct error {
  error_code code;
  std::size_t offset;  // in bytes from the start of the input

  friend constexpr bool operator==(const error&, const error&) = default;
};

// What the parser holds to at most, against input that would exhaust it.
struct limits {
  std::size_t depth = 64;               // elements open at once
  std::size_t token = 1 << 20;          // bytes of one tag, or of text
  std::size_t attributes = 256;         // attributes of one element
};

// A code unit of UTF-8: what the parser reads.
template <class Unit>
concept byte_unit = std::same_as<std::remove_cv_t<Unit>, char> ||
                    std::same_as<std::remove_cv_t<Unit>, char8_t> ||
                    std::same_as<std::remove_cv_t<Unit>, unsigned char> ||
                    std::same_as<std::remove_cv_t<Unit>, std::byte>;

}  // namespace chevron

namespace chevron::detail {

inline constexpr std::string_view xml_uri = "http://www.w3.org/XML/1998/namespace";
inline constexpr std::string_view xmlns_uri = "http://www.w3.org/2000/xmlns/";

// Char, XML 1.0 section 2.2.
constexpr bool character(char32_t cp) noexcept {
  return cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) ||
         (cp >= 0xE000 && cp <= 0xFFFD) || (cp >= 0x10000 && cp <= 0x10FFFF);
}

// NameStartChar and NameChar, XML 1.0 section 2.3, without ':' -- a colon
// is dealt with by QName.
constexpr bool name_start(char32_t cp) noexcept {
  return (cp >= 'A' && cp <= 'Z') || cp == '_' || (cp >= 'a' && cp <= 'z') ||
         (cp >= 0xC0 && cp <= 0xD6) || (cp >= 0xD8 && cp <= 0xF6) || (cp >= 0xF8 && cp <= 0x2FF) ||
         (cp >= 0x370 && cp <= 0x37D) || (cp >= 0x37F && cp <= 0x1FFF) ||
         (cp >= 0x200C && cp <= 0x200D) || (cp >= 0x2070 && cp <= 0x218F) ||
         (cp >= 0x2C00 && cp <= 0x2FEF) || (cp >= 0x3001 && cp <= 0xD7FF) ||
         (cp >= 0xF900 && cp <= 0xFDCF) || (cp >= 0xFDF0 && cp <= 0xFFFD) ||
         (cp >= 0x10000 && cp <= 0xEFFFF);
}

constexpr bool name_char(char32_t cp) noexcept {
  return name_start(cp) || cp == '-' || cp == '.' || (cp >= '0' && cp <= '9') || cp == 0xB7 ||
         (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x203F && cp <= 0x2040);
}

constexpr bool space(char one) noexcept {
  return one == ' ' || one == '\t' || one == '\n' || one == '\r';
}

// One code point of UTF-8 at `at`, strictly: no overlong forms, surrogates
// or values past U+10FFFF. Its length, or 0 where the bytes are not UTF-8.
struct decoded {
  char32_t cp = 0;
  std::size_t length = 0;
};

constexpr decoded decode(std::string_view text, std::size_t at) noexcept {
  const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
  const unsigned char lead = byte(at);
  if (lead < 0x80)
    return {lead, 1};
  std::size_t length = 0;
  char32_t cp = 0;
  char32_t least = 0;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    cp = lead & 0x1F;
    least = 0x80;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    cp = lead & 0x0F;
    least = 0x800;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    cp = lead & 0x07;
    least = 0x10000;
  } else {
    return {};
  }
  if (at + length > text.size())
    return {};
  for (std::size_t k = 1; k < length; ++k) {
    if ((byte(at + k) & 0xC0) != 0x80)
      return {};
    cp = (cp << 6) | (byte(at + k) & 0x3F);
  }
  if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
    return {};
  return {cp, length};
}

constexpr void encode(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

}  // namespace chevron::detail

// How many bytes from `at` are ASCII characters that stand for themselves --
// printable, a tab or a line feed -- found 32 bytes at a time where the text
// is not being read while compiling: Clang's vector types, SSE or AVX on x86
// and NEON on ARM from the same code.
namespace runs {

using bytes32 = unsigned char __attribute__((vector_size(32)));
using flags32 = bool __attribute__((ext_vector_type(32)));

// Taken by reference: a 32-byte vector passed by value is passed in an AVX
// register where there is AVX and on the stack where there is not, so its
// passing is an ABI that depends on -mavx (clang warns -Wpsabi). Inlined,
// this costs nothing.
inline std::uint32_t mask_of(const auto& flags) {
  return __builtin_bit_cast(std::uint32_t, __builtin_convertvector(flags, flags32));
}

constexpr bool plain(unsigned char byte) {
  return (byte >= 0x20 && byte < 0x80) || byte == '\t' || byte == '\n';
}

constexpr std::size_t plain_ascii(std::string_view in, std::size_t at, std::size_t to) {
  std::size_t done = at;
  if !consteval {
    while (done + 32 <= to) {
      bytes32 chunk;
      std::memcpy(&chunk, in.data() + done, 32);
      const std::uint32_t stop =
          (mask_of(chunk < static_cast<unsigned char>(0x20)) &
           ~mask_of(chunk == static_cast<unsigned char>('\t')) &
           ~mask_of(chunk == static_cast<unsigned char>('\n'))) |
          mask_of(chunk >= static_cast<unsigned char>(0x80));
      if (stop != 0) return done + static_cast<std::size_t>(std::countr_zero(stop));
      done += 32;
    }
  }
  while (done < to && plain(static_cast<unsigned char>(in[done]))) ++done;
  return done;
}


// Where the first byte is that is not simply a character: a control other
// than a tab or a line feed -- a carriage return among them, which becomes a
// line feed. 32 bytes at a time.
inline std::size_t special(std::string_view in, std::size_t at, std::size_t to) {
  std::size_t done = at;
  while (done + 32 <= to) {
    bytes32 chunk;
    std::memcpy(&chunk, in.data() + done, 32);
    const std::uint32_t stop = mask_of(chunk < static_cast<unsigned char>(0x20)) &
                               ~mask_of(chunk == static_cast<unsigned char>('\t')) &
                               ~mask_of(chunk == static_cast<unsigned char>('\n'));
    if (stop != 0) return done + static_cast<std::size_t>(std::countr_zero(stop));
    done += 32;
  }
  while (done < to) {
    const auto byte = static_cast<unsigned char>(in[done]);
    if (byte < 0x20 && byte != '\t' && byte != '\n') break;
    ++done;
  }
  return done;
}

// UTF-8 checked 16 bytes at a time, the way simdjson does (Keiser and
// Lemire): each byte looked up by its high nibble and the nibbles of the
// byte before it, the answers ANDed so that every error leaves a bit, and the
// bytes after a three- or four-byte lead checked to be continuations. Byte
// shuffles, SSSE3: compiled for it, used where the processor has it.
#if defined(__x86_64__) || defined(__i386__)
using sbytes16 = char __attribute__((vector_size(16)));
using bytes16 = unsigned char __attribute__((vector_size(16)));

[[gnu::target("ssse3")]] inline bytes16 lookup16(bytes16 table, bytes16 index) {
  return (bytes16)__builtin_ia32_pshufb128((sbytes16)table, (sbytes16)index);
}

template <int Shift>
[[gnu::target("ssse3")]] inline bytes16 before(bytes16 input, bytes16 previous) {
  return (bytes16)__builtin_ia32_palignr128((sbytes16)input, (sbytes16)previous, 16 - Shift);
}

[[gnu::target("ssse3")]] inline bytes16 block_errors(bytes16 input, bytes16 previous) {
  constexpr unsigned char too_short = 1 << 0, too_long = 1 << 1, overlong_3 = 1 << 2,
                          too_large = 1 << 3, surrogate = 1 << 4, overlong_2 = 1 << 5,
                          too_large_1000 = 1 << 6, overlong_4 = 1 << 6, two_conts = 1 << 7,
                          carry = too_short | too_long | two_conts;
  const bytes16 prev1 = before<1>(input, previous);
  constexpr bytes16 first_high = {
      too_long, too_long, too_long, too_long, too_long, too_long, too_long, too_long,
      two_conts, two_conts, two_conts, two_conts,
      too_short | overlong_2, too_short, too_short | overlong_3 | surrogate,
      too_short | too_large | too_large_1000 | overlong_4};
  constexpr bytes16 first_low = {
      carry | overlong_3 | overlong_2 | overlong_4, carry | overlong_2, carry, carry,
      carry | too_large, carry | too_large | too_large_1000,
      carry | too_large | too_large_1000, carry | too_large | too_large_1000,
      carry | too_large | too_large_1000, carry | too_large | too_large_1000,
      carry | too_large | too_large_1000, carry | too_large | too_large_1000,
      carry | too_large | too_large_1000, carry | too_large | too_large_1000 | surrogate,
      carry | too_large | too_large_1000, carry | too_large | too_large_1000};
  constexpr bytes16 second_high = {
      too_short, too_short, too_short, too_short, too_short, too_short, too_short, too_short,
      too_long | overlong_2 | two_conts | overlong_3 | too_large_1000 | overlong_4,
      too_long | overlong_2 | two_conts | overlong_3 | too_large,
      too_long | overlong_2 | two_conts | surrogate | too_large,
      too_long | overlong_2 | two_conts | surrogate | too_large,
      too_short, too_short, too_short, too_short};
  const bytes16 special_cases = lookup16(first_high, prev1 >> 4) &
                                lookup16(first_low, prev1 & static_cast<unsigned char>(0x0f)) &
                                lookup16(second_high, input >> 4);
  const bytes16 third = __builtin_elementwise_sub_sat(
      before<2>(input, previous), bytes16{} + static_cast<unsigned char>(0xe0 - 0x80));
  const bytes16 fourth = __builtin_elementwise_sub_sat(
      before<3>(input, previous), bytes16{} + static_cast<unsigned char>(0xf0 - 0x80));
  return ((third | fourth) & static_cast<unsigned char>(0x80)) ^ special_cases;
}

[[gnu::target("ssse3")]] inline bool utf8_valid_ssse3(std::string_view text) {
  bytes16 previous = {};
  bytes16 errors = {};
  std::size_t done = 0;
  for (; done + 16 <= text.size(); done += 16) {
    bytes16 input;
    std::memcpy(&input, text.data() + done, 16);
    errors |= block_errors(input, previous);
    previous = input;
  }
  if (done != text.size()) {
    bytes16 input = {};
    std::memcpy(&input, text.data() + done, text.size() - done);
    errors |= block_errors(input, previous);
    previous = input;
  }
  errors |= block_errors(bytes16{}, previous);
  return __builtin_reduce_or(errors) == 0;
}

inline bool has_ssse3() {
  static const bool has = __builtin_cpu_supports("ssse3");
  return has;
}
#endif

// Whether a stretch is all characters as they are: well-formed UTF-8, and
// neither U+FFFE nor U+FFFF, the two non-characters UTF-8 can say. False
// where it cannot tell at once; the stretch is then read a character at a
// time, which says where.
inline bool all_characters(std::string_view text) {
#if defined(__x86_64__) || defined(__i386__)
  if (!has_ssse3() || !utf8_valid_ssse3(text)) return false;
  return text.find("\xef\xbf\xbe") == std::string_view::npos &&
         text.find("\xef\xbf\xbf") == std::string_view::npos;
#else
  static_cast<void>(text);
  return false;
#endif
}

}  // namespace runs

// Text an event refers to, kept until the next event: copied into blocks that
// are never moved, so what was handed out stays where it is as more is kept,
// and never freed until the parser is -- clearing only starts again at the
// first block, so a parser that has run a while allocates nothing more. A
// deque of strings did the first; it did not do the second, and it is not
// constexpr.
class text_store {
 public:
  constexpr std::string_view keep(std::string_view text) {
    for (; block_ != blocks_.size(); ++block_, used_ = 0) {
      if (sizes_[block_] - used_ >= text.size()) return put(text);
    }
    const std::size_t size = std::max(text.size(), std::size_t{4096});
    blocks_.push_back(std::make_unique<char[]>(size));
    sizes_.push_back(size);
    used_ = 0;
    return put(text);
  }

  constexpr void clear() {
    block_ = 0;
    used_ = 0;
  }

 private:
  constexpr std::string_view put(std::string_view text) {
    char* const at = blocks_[block_].get() + used_;
    std::ranges::copy(text, at);
    used_ += text.size();
    return {at, text.size()};
  }

  std::vector<std::unique_ptr<char[]>> blocks_;
  std::vector<std::size_t> sizes_;
  std::size_t block_ = 0;
  std::size_t used_ = 0;
};

export namespace chevron {

class parser {
 public:
  constexpr parser() = default;
  constexpr explicit parser(limits held) : limits_(held) {}

  // More of the input, in a piece of any size, split anywhere -- inside a
  // tag, a reference or a UTF-8 sequence as well.
  template <std::ranges::input_range Range>
    requires byte_unit<std::ranges::range_value_t<Range>>
  constexpr void feed(Range&& bytes) {
    for (auto&& one : bytes)
      buffer_.push_back(static_cast<char>(one));
  }

  // The input has ended: what is still open is an error, not a wait.
  constexpr void finish() noexcept { finished_ = true; }

  // What was fed and not read yet. Where a new document follows in the same
  // bytes -- an XMPP stream restarted after authentication -- it is fed to
  // the parser that reads that one.
  constexpr std::string_view unread() const noexcept { return std::string_view(buffer_).substr(at_); }

  // The next event; nothing where the input fed so far holds no complete one
  // -- or, after finish(), where the document is over; or the error. After
  // an error, the same error again.
  constexpr std::expected<std::optional<event>, error> next() {
    if (failed_)
      return std::unexpected(*failed_);
    auto result = step();
    if (!result)
      failed_ = result.error();
    return result;
  }

  // Elements open now.
  constexpr std::size_t depth() const noexcept { return open_.size(); }

 private:
  struct open_element {
    std::string raw;          // the name as written, for the end tag
    std::size_t bindings = 0;  // bindings_ before this element's own
  };
  struct raw_attribute {
    std::string_view name;
    std::string_view value;  // references resolved
  };
  using result = std::expected<std::optional<event>, error>;

  constexpr std::unexpected<error> fail(error_code code, std::size_t at) const {
    return std::unexpected(error{code, base_ + at});
  }

  // Somewhere lasting for a string an event refers to, until the next call.
  constexpr std::string_view keep(std::string_view text) {
    return kept_.keep(text);
  }

  constexpr std::optional<std::string_view> uri_of(std::string_view prefix) const {
    for (auto binding = bindings_.rbegin(); binding != bindings_.rend(); ++binding)
      if (binding->first == prefix)
        return std::string_view(binding->second);
    if (prefix == "xml")
      return detail::xml_uri;
    if (prefix.empty())
      return std::string_view();
    return std::nullopt;
  }

  constexpr result step() {
    kept_.clear();
    if (pending_end_) {
      pending_end_ = false;
      return close_top();
    }
    buffer_.erase(0, at_);
    base_ += at_;
    at_ = 0;
    const std::string_view in = buffer_;
    if (in.empty()) {
      if (!finished_)
        return std::nullopt;
      if (!open_.empty() || !seen_root_)
        return fail(error_code::unexpected_end, 0);
      return std::nullopt;
    }
    if (in[0] != '<')
      return character_data(in);
    if (in.size() < 2)
      return wait(in);
    if (in[1] == '?')
      return question(in);
    if (in[1] == '!')
      return bang(in);
    if (in[1] == '/')
      return end_tag(in);
    return start_tag(in);
  }

  // Not enough of the input yet: wait for more, unless there will be none or
  // a token is already past the limit.
  constexpr result wait(std::string_view in) const {
    if (finished_)
      return fail(error_code::unexpected_end, in.size());
    if (in.size() > limits_.token)
      return fail(error_code::too_large, 0);
    return std::nullopt;
  }

  // An XML declaration, at the very start only; any other <? is a
  // processing instruction.
  constexpr result question(std::string_view in) {
    if (base_ != 0 || !in.starts_with("<?xml") || (in.size() > 5 && !detail::space(in[5]))) {
      if (in.size() < 6 && std::string_view("<?xml ").starts_with(in) && base_ == 0)
        return wait(in);
      return fail(error_code::processing_instruction, 0);
    }
    const std::size_t end = in.find("?>");
    if (end == std::string_view::npos)
      return wait(in);
    if (!declaration(in.substr(5, end - 5)))
      return fail(error_code::bad_declaration, 0);
    at_ = end + 2;
    return step();
  }

  // VersionInfo, then EncodingDecl and SDDecl if there: version 1.0, and an
  // encoding of UTF-8 if one is said.
  static constexpr bool declaration(std::string_view body) {
    std::size_t at = 0;
    bool version = false;
    int seen = 0;
    for (;;) {
      const std::size_t before = at;
      while (at < body.size() && detail::space(body[at]))
        ++at;
      if (at == body.size())
        return version;
      if (at == before)
        return false;
      const std::size_t equals = body.find('=', at);
      if (equals == std::string_view::npos)
        return false;
      std::string_view name = body.substr(at, equals - at);
      while (!name.empty() && detail::space(name.back()))
        name.remove_suffix(1);
      at = equals + 1;
      while (at < body.size() && detail::space(body[at]))
        ++at;
      if (at == body.size() || (body[at] != '"' && body[at] != '\''))
        return false;
      const std::size_t close = body.find(body[at], at + 1);
      if (close == std::string_view::npos)
        return false;
      const std::string_view value = body.substr(at + 1, close - at - 1);
      at = close + 1;
      if (name == "version" && seen == 0) {
        if (value != "1.0")
          return false;
        version = true;
        seen = 1;
      } else if (name == "encoding" && seen == 1) {
        std::string lower(value);
        for (char& one : lower)
          if (one >= 'A' && one <= 'Z')
            one = static_cast<char>(one - 'A' + 'a');
        if (lower != "utf-8")
          return false;
        seen = 2;
      } else if (name == "standalone" && seen >= 1 && seen < 3) {
        if (value != "yes" && value != "no")
          return false;
        seen = 3;
      } else {
        return false;
      }
    }
  }

  // <!--, <![CDATA[ and <!DOCTYPE: only CDATA sections, inside the root.
  constexpr result bang(std::string_view in) {
    constexpr std::string_view cdata = "<![CDATA[";
    if (in.starts_with("<!--"))
      return fail(error_code::comment, 0);
    if (in.starts_with("<!DOCTYPE"))
      return fail(error_code::document_type, 0);
    if (in.starts_with(cdata)) {
      if (open_.empty())
        return fail(error_code::text_outside_root, 0);
      const std::size_t end = in.find("]]>", cdata.size());
      if (end == std::string_view::npos)
        return wait(in);
      std::string content;
      if (const auto bad = checked_characters(in, cdata.size(), end, content))
        return std::unexpected(*bad);
      at_ = end + 3;
      return text{keep(std::move(content))};
    }
    if (cdata.starts_with(in) || std::string_view("<!--").starts_with(in) ||
        std::string_view("<!DOCTYPE").starts_with(in))
      return wait(in);
    return fail(error_code::unexpected_character, 1);
  }

  // Code points of in[from, to) into `out` as they are, line ends made LF;
  // an error where one is not UTF-8 or not a Char.
  constexpr std::optional<error> checked_characters(std::string_view in, std::size_t from,
                                                    std::size_t to, std::string& out) const {
    for (std::size_t at = from; at < to;) {
      // A stretch up to the next control, UTF-8 checked 16 bytes at a time,
      // appended at once.
      if !consteval {
        const std::size_t stop = runs::special(in, at, to);
        if (stop - at >= 16 && runs::all_characters(in.substr(at, stop - at))) {
          out.append(in.substr(at, stop - at));
          at = stop;
          continue;
        }
      }
      // ASCII that needs nothing done to it, appended as a run.
      if (const std::size_t run = runs::plain_ascii(in, at, to); run != at) {
        out.append(in.substr(at, run - at));
        at = run;
        continue;
      }
      const detail::decoded one = detail::decode(in.substr(0, to), at);
      if (one.length == 0)
        return error{error_code::ill_formed_utf8, base_ + at};
      if (!detail::character(one.cp))
        return error{error_code::not_a_character, base_ + at};
      if (one.cp == '\r') {
        out.push_back('\n');
        if (at + 1 < to && in[at + 1] == '\n')
          ++at;
      } else {
        out.append(in.substr(at, one.length));
      }
      at += one.length;
    }
    return std::nullopt;
  }

  // A reference at in[at] == '&', resolved into `out`; where it ends, or an
  // error.
  constexpr std::expected<std::size_t, error> reference(std::string_view in, std::size_t at,
                                                        std::size_t to, std::string& out) const {
    const std::size_t end = in.substr(0, to).find(';', at);
    if (end == std::string_view::npos)
      return std::unexpected(error{error_code::bad_reference, base_ + at});
    const std::string_view name = in.substr(at + 1, end - at - 1);
    if (name.starts_with('#')) {
      const bool hex = name.starts_with("#x");
      const std::string_view digits = name.substr(hex ? 2 : 1);
      if (digits.empty() || digits.size() > 8)
        return std::unexpected(error{error_code::bad_reference, base_ + at});
      char32_t cp = 0;
      for (const char one : digits) {
        std::uint32_t d = 0;
        if (one >= '0' && one <= '9')
          d = static_cast<std::uint32_t>(one - '0');
        else if (hex && one >= 'a' && one <= 'f')
          d = static_cast<std::uint32_t>(one - 'a' + 10);
        else if (hex && one >= 'A' && one <= 'F')
          d = static_cast<std::uint32_t>(one - 'A' + 10);
        else
          return std::unexpected(error{error_code::bad_reference, base_ + at});
        cp = cp * (hex ? 16 : 10) + d;
      }
      if (!detail::character(cp))
        return std::unexpected(error{error_code::not_a_character, base_ + at});
      detail::encode(out, cp);
      return end + 1;
    }
    constexpr std::pair<std::string_view, char> predefined[] = {
        {"lt", '<'}, {"gt", '>'}, {"amp", '&'}, {"apos", '\''}, {"quot", '"'}};
    for (const auto& [known, one] : predefined)
      if (name == known) {
        out.push_back(one);
        return end + 1;
      }
    if (name.empty())
      return std::unexpected(error{error_code::bad_reference, base_ + at});
    return std::unexpected(error{error_code::entity, base_ + at});
  }

  // Character data up to the next '<'. Outside the root only white space,
  // which is not reported.
  constexpr result character_data(std::string_view in) {
    const std::size_t end = in.find('<');
    if (open_.empty()) {
      const std::size_t stop = end == std::string_view::npos ? in.size() : end;
      for (std::size_t at = 0; at < stop; ++at)
        if (!detail::space(in[at]))
          return fail(error_code::text_outside_root, at);
      at_ = stop;
      return step();
    }
    if (end == std::string_view::npos)
      return wait(in);
    std::string content;
    content.reserve(end);  // what it will be, give or take the references
    for (std::size_t at = 0; at < end;) {
      if (in[at] == '&') {
        const auto after = reference(in, at, end, content);
        if (!after)
          return std::unexpected(after.error());
        at = *after;
        continue;
      }
      if (in.substr(at, 3) == "]]>")
        return fail(error_code::unexpected_character, at);
      // Up to the next '&' or "]]>": looked for, not stepped to.
      std::size_t run = at;
      for (;;) {
        run = in.substr(0, end).find_first_of("&]", run);
        if (run == std::string_view::npos) {
          run = end;
          break;
        }
        if (in[run] == '&' || in.substr(run, 3) == "]]>")
          break;
        ++run;
      }
      if (const auto bad = checked_characters(in, at, run, content))
        return std::unexpected(*bad);
      at = run;
    }
    at_ = end;
    return text{keep(std::move(content))};
  }

  // A QName at in[at]: where it ends, or 0 where there is none.
  constexpr std::size_t name_end(std::string_view in, std::size_t at, std::size_t to) const {
    std::size_t colon = std::string_view::npos;
    std::size_t k = at;
    while (k < to) {
      if (in[k] == ':') {
        if (colon != std::string_view::npos || k == at)
          return 0;
        colon = k;
        ++k;
        continue;
      }
      const detail::decoded one = detail::decode(in.substr(0, to), k);
      if (one.length == 0)
        return 0;
      if (!(k == at || k == colon + 1 ? detail::name_start(one.cp) : detail::name_char(one.cp)))
        break;
      k += one.length;
    }
    if (k == at || (colon != std::string_view::npos && colon + 1 == k))
      return 0;
    return k;
  }

  // Where a tag that starts at in[0] ends, past its '>', or npos: the first
  // '>' outside a quoted attribute value.
  static constexpr std::size_t tag_end(std::string_view in) {
    char quote = 0;
    for (std::size_t at = 1; at < in.size(); ++at) {
      if (quote) {
        if (in[at] == quote)
          quote = 0;
      } else if (in[at] == '"' || in[at] == '\'') {
        quote = in[at];
      } else if (in[at] == '>') {
        return at + 1;
      }
    }
    return std::string_view::npos;
  }

  constexpr result start_tag(std::string_view in) {
    if (root_ended_)
      return fail(error_code::after_root, 0);
    const std::size_t end = tag_end(in);
    if (end == std::string_view::npos)
      return wait(in);
    const bool empty = in[end - 2] == '/';
    const std::size_t body_end = empty ? end - 2 : end - 1;
    const std::size_t name_to = name_end(in, 1, body_end);
    if (name_to == 0)
      return fail(error_code::bad_name, 1);
    const std::string_view raw = in.substr(1, name_to - 1);
    std::vector<raw_attribute> raws;
    std::size_t at = name_to;
    for (;;) {
      const std::size_t before = at;
      while (at < body_end && detail::space(in[at]))
        ++at;
      if (at == body_end)
        break;
      if (at == before)
        return fail(error_code::unexpected_character, at);
      const std::size_t name_at = at;
      const std::size_t attribute_to = name_end(in, at, body_end);
      if (attribute_to == 0)
        return fail(error_code::bad_name, at);
      const std::string_view name = in.substr(at, attribute_to - at);
      at = attribute_to;
      while (at < body_end && detail::space(in[at]))
        ++at;
      if (at == body_end || in[at] != '=')
        return fail(error_code::unexpected_character, at);
      ++at;
      while (at < body_end && detail::space(in[at]))
        ++at;
      if (at == body_end || (in[at] != '"' && in[at] != '\''))
        return fail(error_code::unexpected_character, at);
      const char quote = in[at];
      const std::size_t close = in.find(quote, at + 1);
      std::string value;
      for (std::size_t k = at + 1; k < close;) {
        if (in[k] == '<')
          return fail(error_code::unexpected_character, k);
        if (in[k] == '&') {
          const auto after = reference(in, k, close, value);
          if (!after)
            return std::unexpected(after.error());
          k = *after;
          continue;
        }
        std::string piece;
        std::size_t run = k;
        while (run < close && in[run] != '&' && in[run] != '<')
          ++run;
        if (const auto bad = checked_characters(in, k, run, piece))
          return std::unexpected(*bad);
        for (char& one : piece)  // attribute-value normalization, section 3.3.3
          if (one == '\t' || one == '\n')
            one = ' ';
        value += piece;
        k = run;
      }
      for (const raw_attribute& seen : raws)
        if (seen.name == name)
          return fail(error_code::duplicate_attribute, name_at);
      raws.push_back({name, keep(std::move(value))});
      if (raws.size() > limits_.attributes)
        return fail(error_code::too_large, name_at);
      at = close + 1;
    }
    if (open_.size() + 1 > limits_.depth)
      return fail(error_code::too_deep, 0);

    // The element's own namespace declarations, then its names by them.
    const std::size_t mark = bindings_.size();
    for (const raw_attribute& one : raws) {
      std::optional<std::string_view> prefix;
      if (one.name == "xmlns")
        prefix = std::string_view();
      else if (one.name.starts_with("xmlns:"))
        prefix = one.name.substr(6);
      if (!prefix)
        continue;
      const bool xml_prefix = *prefix == "xml";
      if (*prefix == "xmlns" || one.value == detail::xmlns_uri ||
          (xml_prefix != (one.value == detail::xml_uri)) || (!prefix->empty() && one.value.empty()))
        return fail(error_code::bad_namespace, 0);
      bindings_.emplace_back(std::string(*prefix), std::string(one.value));
    }
    const auto resolve = [&](std::string_view name, bool element) -> std::optional<qname> {
      const std::size_t colon = name.find(':');
      if (colon == std::string_view::npos) {
        if (!element)
          return qname{{}, keep(std::string(name))};
        const auto uri = uri_of({});
        return qname{keep(std::string(*uri)), keep(std::string(name))};
      }
      const auto uri = uri_of(name.substr(0, colon));
      if (!uri)
        return std::nullopt;
      return qname{keep(std::string(*uri)), keep(std::string(name.substr(colon + 1)))};
    };
    const auto element = resolve(raw, true);
    if (!element) {
      bindings_.resize(mark);
      return fail(error_code::unbound_prefix, 1);
    }
    attributes_.clear();
    for (const raw_attribute& one : raws) {
      if (one.name == "xmlns" || one.name.starts_with("xmlns:"))
        continue;
      const auto name = resolve(one.name, false);
      if (!name) {
        bindings_.resize(mark);
        return fail(error_code::unbound_prefix, 0);
      }
      for (const attribute& seen : attributes_)
        if (seen.name == *name) {
          bindings_.resize(mark);
          return fail(error_code::duplicate_attribute, 0);
        }
      attributes_.push_back({*name, one.value});
    }
    open_.push_back({std::string(raw), mark});
    seen_root_ = true;
    at_ = end;
    pending_end_ = empty;
    return start_element{*element, attributes_};
  }

  constexpr result end_tag(std::string_view in) {
    const std::size_t end = in.find('>');
    if (end == std::string_view::npos)
      return wait(in);
    const std::size_t name_to = name_end(in, 2, end);
    if (name_to == 0)
      return fail(error_code::bad_name, 2);
    for (std::size_t at = name_to; at < end; ++at)
      if (!detail::space(in[at]))
        return fail(error_code::unexpected_character, at);
    if (open_.empty() || open_.back().raw != in.substr(2, name_to - 2))
      return fail(error_code::mismatched_end_tag, 0);
    at_ = end + 1;
    return close_top();
  }

  // The end of the innermost element: its name by its own bindings, which go
  // with it.
  constexpr result close_top() {
    const open_element top = open_.back();
    const std::size_t colon = top.raw.find(':');
    const std::string_view prefix =
        colon == std::string::npos ? std::string_view() : std::string_view(top.raw).substr(0, colon);
    const qname name{keep(std::string(*uri_of(prefix))),
                     keep(colon == std::string::npos ? top.raw : top.raw.substr(colon + 1))};
    bindings_.resize(top.bindings);
    open_.pop_back();
    if (open_.empty())
      root_ended_ = true;
    return end_element{name};
  }

  limits limits_{};
  std::string buffer_;
  std::size_t at_ = 0;     // what of buffer_ has been read
  std::size_t base_ = 0;   // where buffer_ starts in the input
  bool finished_ = false;
  bool seen_root_ = false;
  bool root_ended_ = false;
  bool pending_end_ = false;  // an empty-element tag's end, to be reported
  std::optional<error> failed_;
  std::vector<open_element> open_;
  std::vector<std::pair<std::string, std::string>> bindings_;  // prefix, URI
  std::vector<attribute> attributes_;
  text_store kept_;
};

}  // namespace chevron
