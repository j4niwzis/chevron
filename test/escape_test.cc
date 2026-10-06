// chevron.escape: text made safe inside markup -- from any range of
// characters, lazily -- and the one string made of it.
import std;
import splice;
import chevron;
import gtest;

#include "gtest/gtest-macros.h"

using namespace std::literals;

TEST(Escape, CharacterData) {
  EXPECT_EQ(std::ranges::to<std::string>(chevron::escaped_text("a < b & c > d\r"sv)), "a &lt; b &amp; c &gt; d&#xD;");
}

TEST(Escape, AttributeValues) {
  EXPECT_EQ(chevron::escaped("say \"hi\"\t&\n<x>"sv), "say &quot;hi&quot;&#x9;&amp;&#xA;&lt;x&gt;");
}

TEST(Escape, PlainTextIsItself) {
  EXPECT_EQ(chevron::escaped("nothing to do"sv), "nothing to do");
}

// Any range of characters: here, one made on the way, which hands out no
// references into anything.
TEST(Escape, LazyInput) {
  const auto upper = std::views::transform("a<b"sv, [](char c) { return c == 'b' ? 'B' : c; });
  EXPECT_EQ(chevron::escaped(upper), "a&lt;B");
}

TEST(Escape, IsConstant) {
  static_assert(chevron::escaped("<&\""sv) == "&lt;&amp;&quot;");
  SUCCEED();
}

TEST(Escape, SafeInXmlCharacterDataToo) {
  const std::string escaped = chevron::escaped("]]>"sv);
  EXPECT_EQ(escaped, "]]&gt;");
  chevron::parser parser;
  parser.feed("<a>" + escaped + "</a>");
  parser.finish();
  std::string text;
  for (;;) {
    auto next = parser.next();
    ASSERT_TRUE(next.has_value());
    if (!*next) break;
    if (const auto* piece = spl::get_if<chevron::text>(&**next)) text += piece->content;
  }
  EXPECT_EQ(text, "]]>"sv);
}
