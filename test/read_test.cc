// Typed reading: plain structs, schemas beside them, and XML read straight
// into them.
import std;
import chevron;
import gtest;

#include "gtest/gtest-macros.h"

namespace chat {

struct message {
  std::string to, from;
  std::optional<std::string> type;
  std::optional<std::string> body;
  std::vector<chevron::any> extensions;
};

constexpr auto xml_schema(chevron::type<message>) {
  using namespace chevron::members;
  return chevron::schema<message>()
      .name("urn:example:client", "message")
      .member<"to">(attribute())
      .member<"from">(attribute())
      .member<"type">(attribute())
      .member<"body">(child_text())
      .member<"extensions">(unknown_children());
}

struct presence {
  std::optional<std::string> show;
  std::optional<int> priority;
};

// Nothing to say but the name: both members are child elements with text.
constexpr auto xml_schema(chevron::type<presence>) {
  return chevron::schema<presence>().name("urn:example:client", "presence");
}

struct item {
  std::string jid;
  int order;
  std::vector<std::string> group;
};

// Positional: jid and order attributes, group the default.
constexpr auto xml_schema(chevron::type<item>) {
  using namespace chevron::members;
  return chevron::schema<item>().name("urn:example:roster", "item").members(attribute(), attribute(), _);
}

struct query {
  std::vector<item> items;
};

// The member is items; the element is item.
constexpr auto xml_schema(chevron::type<query>) {
  using namespace chevron::members;
  return chevron::schema<query>().name("urn:example:roster", "query").member<"items">(child("item"));
}

}  // namespace chat

namespace {

auto events(std::string_view document) { return document | chevron::events; }

}  // namespace

TEST(Read, AMessage) {
  const auto m = chevron::read<chat::message>(events(
      "<message xmlns='urn:example:client' to='juliet' from='romeo' type='chat'>"
      "<body>Art thou not Romeo?</body>"
      "<x xmlns='urn:example:x' a='1'>kept <y/></x>"
      "</message>"));
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->to, "juliet");
  EXPECT_EQ(m->from, "romeo");
  EXPECT_EQ(m->type, std::optional<std::string>("chat"));
  EXPECT_EQ(m->body, std::optional<std::string>("Art thou not Romeo?"));
  ASSERT_EQ(m->extensions.size(), 1u);
  const chevron::any& x = m->extensions[0];
  EXPECT_EQ(x.uri, "urn:example:x");
  EXPECT_EQ(x.local, "x");
  ASSERT_EQ(x.attributes.size(), 1u);
  EXPECT_EQ(x.attributes[0].second, "1");
  ASSERT_EQ(x.children.size(), 2u);
  EXPECT_EQ(std::get<std::string>(x.children[0].value), "kept ");
  EXPECT_EQ(std::get<chevron::any>(x.children[1].value).local, "y");
}

TEST(Read, OptionalAndMissing) {
  const auto without = chevron::read<chat::message>(
      events("<message xmlns='urn:example:client' to='a' from='b'/>"));
  ASSERT_TRUE(without.has_value());
  EXPECT_FALSE(without->type.has_value());
  EXPECT_FALSE(without->body.has_value());

  const auto missing = chevron::read<chat::message>(events("<message xmlns='urn:example:client' from='b'/>"));
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, chevron::read_code::missing_attribute);
  EXPECT_EQ(missing.error().where, "to");

  const auto other = chevron::read<chat::message>(events("<iq xmlns='urn:example:client'/>"));
  ASSERT_FALSE(other.has_value());
  EXPECT_EQ(other.error().code, chevron::read_code::unexpected_element);
}

TEST(Read, NestedAndRepeated) {
  const auto q = chevron::read<chat::query>(events(
      "<query xmlns='urn:example:roster'>"
      "<item jid='nurse' order='2'><group>Capulets</group><group>Servants</group></item>"
      "<item jid='romeo' order='1'/>"
      "<unrelated/>"
      "</query>"));
  ASSERT_TRUE(q.has_value());
  ASSERT_EQ(q->items.size(), 2u);
  EXPECT_EQ(q->items[0].jid, "nurse");
  EXPECT_EQ(q->items[0].order, 2);
  EXPECT_EQ(q->items[0].group, (std::vector<std::string>{"Capulets", "Servants"}));
  EXPECT_EQ(q->items[1].order, 1);

  const auto bad = chevron::read<chat::query>(
      events("<query xmlns='urn:example:roster'><item jid='a' order='two'/></query>"));
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code, chevron::read_code::bad_value);
  EXPECT_EQ(bad.error().where, "order");
}

TEST(Read, OneOf) {
  const auto p = chevron::read_one_of<chat::message, chat::presence>(
      events("<presence xmlns='urn:example:client'><show>away</show><priority>5</priority></presence>"));
  ASSERT_TRUE(p.has_value());
  ASSERT_EQ(p->index(), 1u);
  EXPECT_EQ(std::get<chat::presence>(*p).show, std::optional<std::string>("away"));
  EXPECT_EQ(std::get<chat::presence>(*p).priority, std::optional<int>(5));
}

TEST(Read, FromAParserFedAsBytesArrive) {
  chevron::parser p;
  p.feed(std::string_view("<message xmlns='urn:example:client' to='a' from='b'><bo"));
  const auto early = chevron::read<chat::message>(p);
  ASSERT_FALSE(early.has_value());
  EXPECT_EQ(early.error().code, chevron::read_code::incomplete);

  chevron::parser q;
  q.feed(std::string_view("<message xmlns='urn:example:client' to='a' from='b'><body>hi</body></message>"));
  const auto whole = chevron::read<chat::message>(q);
  ASSERT_TRUE(whole.has_value());
  EXPECT_EQ(whole->body, std::optional<std::string>("hi"));

  // Not well-formed: the parser's error, carried through.
  const auto broken = chevron::read<chat::message>(events("<message xmlns='urn:example:client' to='a' from='b'><!-- --></message>"));
  ASSERT_FALSE(broken.has_value());
  EXPECT_EQ(broken.error().code, chevron::read_code::parse);
  ASSERT_TRUE(broken.error().parse_error.has_value());
  EXPECT_EQ(broken.error().parse_error->code, chevron::error_code::comment);
}
