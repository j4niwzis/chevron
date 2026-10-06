// The event parser: what it reads, what it refuses, and that it reads the
// same whether the input comes whole, byte by byte, or from a range.
import std;
import splice;
import chevron;
import gtest;

#include "gtest/gtest-macros.h"

namespace {

using chevron::error_code;

// An event as text: S{uri}local[{uri}name=value,...], E{uri}local, T"...";
// an error as !code@offset.
std::string shown(const chevron::event& one) {
  const auto name = [](const chevron::qname& n) {
    return "{" + std::string(n.uri) + "}" + std::string(n.local);
  };
  if (const auto* start = spl::get_if<chevron::start_element>(&one)) {
    std::string out = "S" + name(start->name) + "[";
    for (std::size_t k = 0; k < start->attributes.size(); ++k)
      out += (k ? "," : "") + name(start->attributes[k].name) + "=" + std::string(start->attributes[k].value);
    return out + "]";
  }
  if (const auto* end = spl::get_if<chevron::end_element>(&one))
    return "E" + name(end->name);
  return "T\"" + std::string(spl::get<chevron::text>(one).content) + "\"";
}

std::string shown(const chevron::error& one) {
  return "!" + std::to_string(static_cast<int>(one.code)) + "@" + std::to_string(one.offset);
}

// Everything a document gives, fed in pieces of `piece` bytes, finished or
// left open.
std::vector<std::string> read(std::string_view document, std::size_t piece = std::string_view::npos,
                              bool finish = true, chevron::limits held = {}) {
  chevron::parser p(held);
  std::vector<std::string> out;
  const auto drain = [&] {
    for (;;) {
      const auto next = p.next();
      if (!next) {
        out.push_back(shown(next.error()));
        return false;
      }
      if (!*next)
        return true;
      out.push_back(shown(**next));
    }
  };
  for (std::size_t at = 0; at < document.size(); at += piece) {
    p.feed(document.substr(at, piece));
    if (!drain())
      return out;
  }
  if (finish) {
    p.finish();
    drain();
  }
  return out;
}

using shown_events = std::vector<std::string>;

// The same, read as HTML (dialect::html).
std::vector<std::string> read_html(std::string_view document, std::size_t piece = std::string_view::npos) {
  chevron::parser p(chevron::limits{}, chevron::dialect::html{});
  std::vector<std::string> out;
  const auto drain = [&] {
    for (;;) {
      const auto next = p.next();
      if (!next) {
        out.push_back(shown(next.error()));
        return false;
      }
      if (!*next)
        return true;
      out.push_back(shown(**next));
    }
  };
  for (std::size_t at = 0; at < document.size(); at += piece) {
    p.feed(document.substr(at, piece));
    if (!drain())
      return out;
  }
  p.finish();
  drain();
  return out;
}
// Text events run together, as what is read the same may come in pieces
// that differ as the input does.
std::vector<std::string> joined(std::vector<std::string> events) {
  std::vector<std::string> out;
  for (std::string& one : events)
    if (!out.empty() && out.back().starts_with("T\"") && one.starts_with("T\""))
      out.back() = out.back().substr(0, out.back().size() - 1) + one.substr(2);
    else
      out.push_back(std::move(one));
  return out;
}

std::string error(error_code code, std::size_t offset) {
  return "!" + std::to_string(static_cast<int>(code)) + "@" + std::to_string(offset);
}

}  // namespace

TEST(Parser, ElementsAttributesText) {
  EXPECT_EQ(read("<a x='1' y=\"2\">hi<b/>there</a>"),
            (shown_events{"S{}a[{}x=1,{}y=2]", "T\"hi\"", "S{}b[]", "E{}b", "T\"there\"", "E{}a"}));
  EXPECT_EQ(read("<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n<a/>\n"),
            (shown_events{"S{}a[]", "E{}a"}));
  EXPECT_EQ(read("<данные атр='з'/>"),
            (shown_events{"S{}данные[{}атр=з]", "E{}данные"}));
}

TEST(Parser, Namespaces) {
  const std::string_view stream =
      "<stream:stream xmlns:stream='urn:s' xmlns='urn:c' to='example.com'>"
      "<message to='juliet' xml:lang='en'><body>Hi</body><x xmlns='urn:x' xmlns:p='urn:p' p:a='1'/></message>";
  EXPECT_EQ(read(stream, std::string_view::npos, false),
            (shown_events{"S{urn:s}stream[{}to=example.com]",
                          "S{urn:c}message[{}to=juliet,{http://www.w3.org/XML/1998/namespace}lang=en]",
                          "S{urn:c}body[]", "T\"Hi\"", "E{urn:c}body", "S{urn:x}x[{urn:p}a=1]", "E{urn:x}x",
                          "E{urn:c}message"}));
  // A stream that stays open is waiting, not wrong -- until the input ends.
  EXPECT_EQ(read(stream).back(), error(error_code::unexpected_end, stream.size()));
  EXPECT_EQ(read("<a xmlns='urn:a'><b xmlns=''/></a>"),
            (shown_events{"S{urn:a}a[]", "S{}b[]", "E{}b", "E{urn:a}a"}));
}

TEST(Parser, ReferencesLineEndsCdata) {
  EXPECT_EQ(read("<a t='x&#9;y\tz&lt;'>&lt;&amp;&gt;&apos;&quot;&#x1F600;&#65;</a>"),
            (shown_events{"S{}a[{}t=x\ty z<]", "T\"<&>'\"\U0001F600A\"", "E{}a"}));
  EXPECT_EQ(read("<a>1\r\n2\r3</a>"), (shown_events{"S{}a[]", "T\"1\n2\n3\"", "E{}a"}));
  EXPECT_EQ(read("<a><![CDATA[<b>&amp;]]></a>"), (shown_events{"S{}a[]", "T\"<b>&amp;\"", "E{}a"}));
}

TEST(Parser, WhatItRefuses) {
  struct refused {
    std::string_view document;
    error_code code;
    std::size_t offset;
  };
  const refused all[] = {
      {"<a><!-- no --></a>", error_code::comment, 3},
      {"<a><?pi?></a>", error_code::processing_instruction, 3},
      {"<!DOCTYPE a><a/>", error_code::document_type, 0},
      {"<a>&nbsp;</a>", error_code::entity, 3},
      {"<a>&#0;</a>", error_code::not_a_character, 3},
      {"<a>&#xD800;</a>", error_code::not_a_character, 3},
      {"<a>&amp</a>", error_code::bad_reference, 3},
      {"<a></b>", error_code::mismatched_end_tag, 3},
      {"<p:a/>", error_code::unbound_prefix, 1},
      {"<a x='1' x='2'/>", error_code::duplicate_attribute, 9},
      {"<a xmlns:p='u' xmlns:q='u' p:x='1' q:x='2'/>", error_code::duplicate_attribute, 0},
      {"<a/><b/>", error_code::after_root, 4},
      {"x<a/>", error_code::text_outside_root, 0},
      {"<a>\xC3</a>", error_code::ill_formed_utf8, 3},
      {"<a>\xC0\xAF</a>", error_code::ill_formed_utf8, 3},
      {"<1a/>", error_code::bad_name, 1},
      {"<a x=1/>", error_code::unexpected_character, 5},
      {"<a x='<'/>", error_code::unexpected_character, 6},
      {"<a>]]></a>", error_code::unexpected_character, 3},
      {"<?xml version='1.1'?><a/>", error_code::bad_declaration, 0},
      {"<?xml version='1.0' encoding='latin1'?><a/>", error_code::bad_declaration, 0},
      {"<a/><?xml version='1.0'?>", error_code::processing_instruction, 4},
      {"<a xmlns:xml='urn:x'/>", error_code::bad_namespace, 0},
      {"<a xmlns:p=''/>", error_code::bad_namespace, 0},
      {"<a:b:c/>", error_code::bad_name, 1},
  };
  for (const refused& one : all)
    EXPECT_EQ(read(one.document).back(), error(one.code, one.offset)) << one.document;
  EXPECT_EQ(read("<a><b><c/></b></a>", std::string_view::npos, true, {.depth = 2}).back(),
            error(error_code::too_deep, 6));
}

// Every document gives the same, fed whole or one byte at a time or in pieces
// of three -- errors and their offsets included.
TEST(Parser, TheSameHoweverItIsSplit) {
  const std::string_view documents[] = {
      "<?xml version='1.0'?><stream:stream xmlns:stream='urn:s' xmlns='urn:c'><message><body>é &amp; \U0001F600</body></message>",
      "<a x='1&#x20;2'>t<![CDATA[c]]>\r\n<b/></a>",
      "<a>&nbsp;</a>",
      "<a><!-- --></a>",
      "<a>\xE2\x82</a>",
  };
  for (const std::string_view document : documents) {
    const auto whole = read(document);
    EXPECT_EQ(read(document, 1), whole) << document;
    EXPECT_EQ(read(document, 3), whole) << document;
  }
}

// What was fed past the end of what was read, for the parser of the next
// document.
TEST(Parser, Unread) {
  chevron::parser p;
  p.feed(std::string_view("<a/><?xml version='1.0'?><b"));
  ASSERT_TRUE(p.next().has_value());  // <a>
  ASSERT_TRUE(p.next().has_value());  // </a>
  EXPECT_EQ(p.unread(), "<?xml version='1.0'?><b");
}

TEST(Parser, FromRanges) {
  const std::string document = "<a x='1'>hi<b/></a>";
  std::vector<std::string> from_view;
  for (const auto& one : std::string_view(document) | chevron::events)
    from_view.push_back(one ? shown(*one) : shown(one.error()));
  EXPECT_EQ(from_view, read(document));

  // A range read only once: a stream.
  std::istringstream stream(document);
  stream >> std::noskipws;
  std::vector<std::string> from_stream;
  for (const auto& one : std::views::istream<char>(stream) | chevron::events)
    from_stream.push_back(one ? shown(*one) : shown(one.error()));
  EXPECT_EQ(from_stream, read(document));

  // An error ends the view.
  std::vector<std::string> refused;
  for (const auto& one : std::string_view("<a><!----></a>") | chevron::events)
    refused.push_back(one ? shown(*one) : shown(one.error()));
  EXPECT_EQ(refused, (shown_events{"S{}a[]", error(error_code::comment, 3)}));
}

namespace {

// Input as a socket gives it: asking whether it has ended waits for the peer.
struct live_input {
  std::string_view data;
  std::size_t at = 0;
  bool asked_past = false;

  struct iterator {
    using value_type = char;
    using difference_type = std::ptrdiff_t;
    live_input* in = nullptr;
    char operator*() const { return in->data[in->at]; }
    iterator& operator++() {
      ++in->at;
      return *this;
    }
    void operator++(int) { ++*this; }
    bool operator==(std::default_sentinel_t) const {
      if (in->at == in->data.size())
        in->asked_past = true;
      return in->at == in->data.size();
    }
  };
  iterator begin() { return {this}; }
  std::default_sentinel_t end() const { return {}; }
};

}  // namespace

// Units read one by one, to the '>' that ends an event and no further.
TEST(Parser, NothingReadAhead) {
  live_input input{"<a><b x='1'>hi</b>"};
  std::size_t ends = 0;
  for (auto&& one : std::ranges::ref_view(input) | chevron::events) {
    ASSERT_TRUE(one.has_value());
    if (spl::holds_alternative<chevron::end_element>(*one) && ++ends == 1)
      break;
  }
  EXPECT_EQ(ends, 1u);
  EXPECT_FALSE(input.asked_past);
}

// Chunks taken whole -- split anywhere, inside a tag or a UTF-8 sequence --
// or made by chunked<N>: the same events.
TEST(Parser, FromChunks) {
  const std::string document = "<a x='1'>h\xd0\x9f\xd1\x80i<b/></a>";
  const auto shown_all = [](auto&& range) {
    std::vector<std::string> out;
    for (const auto& one : std::forward<decltype(range)>(range) | chevron::events)
      out.push_back(one ? shown(*one) : shown(one.error()));
    return out;
  };
  const auto whole = read(document);
  const std::vector<std::string> pieces{"<a x", "='1'>h\xd0", "\x9f\xd1\x80i<", "b/></a>"};
  EXPECT_EQ(shown_all(pieces), whole);
  EXPECT_EQ(shown_all(std::string_view(document) | chevron::chunked<1>), whole);
  EXPECT_EQ(shown_all(std::string_view(document) | chevron::chunked<5>), whole);
  EXPECT_EQ(shown_all(std::string_view(document) | chevron::chunked<4096>), whole);
  std::istringstream stream(document);
  stream >> std::noskipws;
  EXPECT_EQ(shown_all(std::views::istream<char>(stream) | chevron::chunked<3>), whole);
}

namespace {

// Long enough for the checks 16 bytes at a time, with what they must refuse
// at every place.
TEST(Parser, LongTextChecked) {
  const auto read = [](const std::string& body) {
    const std::string document = "<a>" + body + "</a>";
    std::string text;
    for (auto&& one : std::string_view(document) | chevron::events) {
      if (!one) return std::optional<std::string>();
      if (const auto* piece = spl::get_if<chevron::text>(&*one)) text += piece->content;
    }
    return std::optional<std::string>(text);
  };
  const std::string cyrillic = "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 ";
  for (std::size_t at = 0; at != 40; ++at) {
    const std::string before = std::string(at, 'a') + cyrillic;
    const std::string after = cyrillic + std::string(20, 'b');
    EXPECT_EQ(read(before + "\xf0\x9f\x98\x80" + after), before + "\xf0\x9f\x98\x80" + after);
    EXPECT_FALSE(read(before + "\xef\xbf\xbe" + after)) << at;  // U+FFFE
    EXPECT_FALSE(read(before + "\xef\xbf\xbf" + after)) << at;  // U+FFFF
    EXPECT_FALSE(read(before + "\xc0\x80" + after)) << at;      // overlong
    EXPECT_FALSE(read(before + "\xed\xa0\x80" + after)) << at;  // a surrogate
    EXPECT_FALSE(read(before + "\xd0" + after)) << at;          // cut short
    EXPECT_FALSE(read(before + "\x01" + after)) << at;          // a control
    EXPECT_EQ(read(before + "\r\n" + after), before + "\n" + after);
  }
}

}  // namespace

TEST(Html, AFragmentWithTextAndElementsAtTheTop) {
  EXPECT_EQ(read_html("<b>bold</b> <i>it</i><br>next<br/>line"),
            (shown_events{"S{}b[]", "T\"bold\"", "E{}b", "T\" \"", "S{}i[]", "T\"it\"", "E{}i", "S{}br[]", "E{}br",
                          "T\"next\"", "S{}br[]", "E{}br", "T\"line\""}));
}

TEST(Html, ReferencesAndStrayAmpersandsAndLessThans) {
  EXPECT_EQ(joined(read_html("a &amp; &mdash; &#8212; &#x1F600; &bogus; b & c < d")),
            (shown_events{"T\"a & \u2014 \u2014 \U0001F600 &bogus; b & c < d\""}));
}

TEST(Html, NamesInLowerCaseAttributesUnquotedAndBare) {
  EXPECT_EQ(read_html("<IMG data-mx-emoticon SRC=mxc://a/b ALT=\":cat:\" height=32 src='again'>"),
            (shown_events{"S{}img[{}data-mx-emoticon=,{}src=mxc://a/b,{}alt=:cat:,{}height=32]", "E{}img"}));
  EXPECT_EQ(read_html("<a href=\"https://e.com/?a=1&amp;b=2\" title=\"x > y\">l</a>"),
            (shown_events{"S{}a[{}href=https://e.com/?a=1&b=2,{}title=x > y]", "T\"l\"", "E{}a"}));
}

TEST(Html, ImpliedAndStrayEndTags) {
  EXPECT_EQ(read_html("<p>one<p>two<ul><li>a<li>b</ul>"),
            (shown_events{"S{}p[]", "T\"one\"", "E{}p", "S{}p[]", "T\"two\"", "E{}p", "S{}ul[]", "S{}li[]", "T\"a\"",
                          "E{}li", "S{}li[]", "T\"b\"", "E{}li", "E{}ul"}));
  EXPECT_EQ(read_html("<i>x</span>y</i><b>open"),
            (shown_events{"S{}i[]", "T\"x\"", "T\"y\"", "E{}i", "S{}b[]", "T\"open\"", "E{}b"}));
  EXPECT_EQ(read_html("<a><b><c>x</a>"), (shown_events{"S{}a[]", "S{}b[]", "S{}c[]", "T\"x\"", "E{}c", "E{}b", "E{}a"}));
}

TEST(Html, CommentsPassedOverAndAnUnclosedTagAsText) {
  EXPECT_EQ(joined(read_html("a<!-- c -->b<!DOCTYPE html>c <unclosed")), (shown_events{"T\"abc <unclosed\""}));
}

TEST(Html, TheSameByteByByte) {
  for (const std::string_view html : {std::string_view("<p>one<p>two &amp; <b>x</b><br>y < z &#x1F600;"),
                                      std::string_view("<mx-reply><blockquote>q</blockquote></mx-reply>a<!--c-->b")})
    EXPECT_EQ(joined(read_html(html, 1)), joined(read_html(html)));
}

TEST(Parser, TokenLimitsDoNotDependOnFeedBoundaries) {
  const chevron::limits limits{.token = 16};
  const std::vector<std::string> documents{
      "<a x='" + std::string(40, 'x') + "'/>",
      "<" + std::string(40, 'x') + "/>",
      "<a>" + std::string(40, 'x') + "</a>",
      "<a><![CDATA[" + std::string(40, 'x') + "]]></a>",
      "<?xml version='1.0'?><a/>",
      "<a></a" + std::string(40, ' ') + ">"};
  for (const auto& document : documents) {
    const auto whole = read(document, std::string_view::npos, true, limits);
    ASSERT_TRUE(whole.back().starts_with("!"));
    for (std::size_t piece : {1u, 2u, 7u, 17u, 32u})
      EXPECT_EQ(read(document, piece, true, limits), whole) << document << " chunk=" << piece;
    EXPECT_EQ(whole.back(), error(error_code::too_large,
        document.starts_with("<a>") ? 3 : 0));
  }
  EXPECT_EQ(read("<a>" + std::string(16, 'x') + "</a>", 1, true, limits).back(), "E{}a");
  std::string many = "<a>";
  for (int i = 0; i != 100; ++i) many += "<b/>";
  many += "</a>";
  EXPECT_EQ(read(many, std::string_view::npos, true, limits).back(), "E{}a");

  chevron::parser incomplete(limits);
  incomplete.feed(std::string_view("<a x='xxxxxxxxxxxxxxxxxxxx"));
  incomplete.finish();
  const auto refused = incomplete.next();
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, error_code::too_large);
}

TEST(Html, TokenLimitsAlsoApplyToCompleteAndRecoveredTokens) {
  const std::vector<std::string> documents{
      "<a x='" + std::string(40, 'x') + "'>",
      "<!--" + std::string(40, 'x') + "-->",
      "<?" + std::string(40, 'x') + ">",
      "</a" + std::string(40, ' ') + ">",
      std::string(40, 'x'), "<a x='" + std::string(40, 'x')};
  for (const auto& document : documents) {
    for (std::size_t chunk : {1u, 5u, 128u}) {
      chevron::parser parser({.token = 16}, chevron::dialect::html{});
      std::optional<chevron::error> error;
      for (std::size_t at = 0; at < document.size() && !error; at += chunk) {
        parser.feed(std::string_view(document).substr(at, chunk));
        auto next = parser.next();
        if (!next) error = next.error();
      }
      if (!error) {
        parser.finish();
        auto next = parser.next();
        if (!next) error = next.error();
      }
      ASSERT_TRUE(error) << document << " chunk=" << chunk;
      EXPECT_EQ(error->code, error_code::too_large);
      EXPECT_EQ(error->offset, 0u);
    }
  }
}

TEST(Parser, LargeInputCompactionPreservesEventsAndOffsets) {
  std::string document = "<a>";
  for (int i = 0; i != 4000; ++i) document += "<b>text</b>";
  document += "</wrong>";
  const auto whole = read(document);
  EXPECT_EQ(whole, read(document, 97));
  EXPECT_EQ(whole.back(), error(error_code::mismatched_end_tag, document.size() - 8));
}
