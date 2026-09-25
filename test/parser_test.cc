// The event parser: what it reads, what it refuses, and that it reads the
// same whether the input comes whole, byte by byte, or from a range.
import std;
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
  if (const auto* start = std::get_if<chevron::start_element>(&one)) {
    std::string out = "S" + name(start->name) + "[";
    for (std::size_t k = 0; k < start->attributes.size(); ++k)
      out += (k ? "," : "") + name(start->attributes[k].name) + "=" + std::string(start->attributes[k].value);
    return out + "]";
  }
  if (const auto* end = std::get_if<chevron::end_element>(&one))
    return "E" + name(end->name);
  return "T\"" + std::string(std::get<chevron::text>(one).content) + "\"";
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
