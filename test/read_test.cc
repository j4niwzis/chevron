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

TEST(Write, ExactOutput) {
  const chat::message m{"juliet", "romeo", std::nullopt, "a < b & \"c\"", {}};
  EXPECT_EQ(chevron::to_xml(m) | std::ranges::to<std::string>(),
            "<message xmlns=\"urn:example:client\" to=\"juliet\" from=\"romeo\">"
            "<body>a &lt; b &amp; \"c\"</body></message>");
  // Attribute values keep their white space and quotes.
  const chat::message odd{"a\"b\tc", "x", "chat", std::nullopt, {}};
  EXPECT_EQ(chevron::to_xml(odd) | std::ranges::to<std::string>(),
            "<message xmlns=\"urn:example:client\" to=\"a&quot;b&#x9;c\" from=\"x\" type=\"chat\"/>");
}

// Written, then read back: the same value, for every kind of member.
TEST(Write, ReadBackTheSame) {
  chat::query q;
  q.items.push_back({"nurse", 2, {"Capulets", "Servants"}});
  q.items.push_back({"romeo", 1, {}});
  const std::string written = chevron::to_xml(q) | std::ranges::to<std::string>();
  const auto back = chevron::read<chat::query>(std::string_view(written) | chevron::events);
  ASSERT_TRUE(back.has_value()) << written;
  ASSERT_EQ(back->items.size(), 2u);
  EXPECT_EQ(back->items[0].jid, "nurse");
  EXPECT_EQ(back->items[0].order, 2);
  EXPECT_EQ(back->items[0].group, (std::vector<std::string>{"Capulets", "Servants"}));
  EXPECT_EQ(back->items[1].group.size(), 0u);

  // An extension kept whole is written back as it came, namespace and all.
  const std::string_view in =
      "<message xmlns=\"urn:example:client\" to=\"a\" from=\"b\"><body>hi</body>"
      "<x xmlns=\"urn:example:x\" a=\"1\">kept <y/></x></message>";
  const auto m = chevron::read<chat::message>(in | chevron::events);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(chevron::to_xml(*m) | std::ranges::to<std::string>(), in);

  const chat::presence p{"away", 5};
  const std::string presence = chevron::to_xml(p) | std::ranges::to<std::string>();
  const auto p_back = chevron::read<chat::presence>(std::string_view(presence) | chevron::events);
  ASSERT_TRUE(p_back.has_value()) << presence;
  EXPECT_EQ(p_back->show, p.show);
  EXPECT_EQ(p_back->priority, p.priority);
}

namespace {

template <class T>
std::string eagerly(const T& value) {
  std::string out;
  chevron::write(std::back_inserter(out), value);
  return out;
}

}  // namespace

// The lazy view gives what writing at once gives, character by character or
// piece by piece, from a value it refers to or one it keeps.
TEST(Write, LazyIsTheSameAsEager) {
  chat::query q;
  for (int n = 0; n < 50; ++n)
    q.items.push_back({"contact" + std::to_string(n), n, {"group <" + std::to_string(n % 3) + ">"}});
  const std::string whole = eagerly(q);
  EXPECT_EQ(chevron::to_xml(q) | std::ranges::to<std::string>(), whole);

  auto view = chevron::to_xml(q);
  std::string joined;
  std::size_t pieces = 0;
  for (const std::string_view one : view.chunks()) {
    joined += one;
    ++pieces;
  }
  EXPECT_EQ(joined, whole);
  EXPECT_GT(pieces, 100u);  // made in pieces, not as one string

  // Kept by the view: the value it came from is gone.
  const std::string kept = chevron::to_xml(chat::presence{"dnd", 7}) | std::ranges::to<std::string>();
  EXPECT_EQ(kept, eagerly(chat::presence{"dnd", 7}));

  // Only as far as it is read: the first few characters, and no more made.
  EXPECT_EQ(chevron::to_xml(q) | std::views::take(6) | std::ranges::to<std::string>(), "<query");
}


namespace {

// Written lazily and read back while the program is compiled.
constexpr bool round_trip() {
  chat::query q;
  q.items.push_back({"nurse", 2, {"Capulets", "Servants"}});
  q.items.push_back({"romeo", 1, {}});
  const std::string written = chevron::to_xml(q) | std::ranges::to<std::string>();
  const auto back = chevron::read<chat::query>(std::string_view(written) | chevron::events);
  return back && back->items.size() == 2 && back->items[0].jid == "nurse" &&
         back->items[0].order == 2 && back->items[0].group.size() == 2 &&
         back->items[1].group.empty();
}

static_assert(round_trip());

TEST(Write, AtCompileTime) { EXPECT_TRUE(round_trip()); }

}  // namespace

namespace chat {

struct note {
  std::string to;
  std::optional<std::string> body;
  chevron::kept_attributes others;
};

constexpr auto xml_schema(chevron::type<note>) {
  using namespace chevron::members;
  return chevron::schema<note>()
      .name("urn:example:client", "note")
      .member<"to">(attribute())
      .member<"body">(child_text())
      .member<"others">(unknown_attributes());
}

}  // namespace chat

namespace {

TEST(Read, KeptAttributes) {
  const std::string_view in =
      R"(<note xmlns="urn:example:client" id="n1" to="juliet" xml:lang="en" )"
      R"(xmlns:x="urn:example:x" x:mark="yes"><body>hi</body></note>)";
  const auto got = chevron::read<chat::note>(in | chevron::events);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->to, "juliet");
  ASSERT_EQ(got->others.size(), 3u);
  EXPECT_EQ(got->others[0].first.second, "id");
  EXPECT_EQ(got->others[1].first.first, "http://www.w3.org/XML/1998/namespace");
  EXPECT_EQ(got->others[2].first.first, "urn:example:x");
  EXPECT_EQ(got->others[2].second, "yes");
  // Written back, lazily and at once alike, and read back the same.
  const std::string lazy = chevron::to_xml(*got) | std::ranges::to<std::string>();
  std::string eager;
  auto out = std::back_inserter(eager);
  chevron::write(out, *got);
  EXPECT_EQ(lazy, eager);
  const auto back = chevron::read<chat::note>(std::string_view(lazy) | chevron::events);
  ASSERT_TRUE(back.has_value()) << lazy;
  EXPECT_EQ(back->others, got->others);
  EXPECT_EQ(back->body, "hi");
}

}  // namespace

namespace chat {

struct normal {
  static constexpr std::string_view xml_value = "normal";
};
struct chat_type {
  static constexpr std::string_view xml_value = "chat";
};
struct groupchat {
  static constexpr std::string_view xml_value = "groupchat";
};

struct typed_note {
  std::optional<std::variant<normal, chat_type, groupchat>> type;
  std::optional<std::string> body;
};

constexpr auto xml_schema(chevron::type<typed_note>) {
  using namespace chevron::members;
  return chevron::schema<typed_note>()
      .name("urn:example:client", "note")
      .member<"type">(attribute())
      .member<"body">(child_text());
}

}  // namespace chat

namespace {

TEST(Read, ChoiceAttribute) {
  const auto got = chevron::read<chat::typed_note>(
      std::string_view(R"(<note xmlns="urn:example:client" type="groupchat"><body>x</body></note>)") |
      chevron::events);
  ASSERT_TRUE(got.has_value());
  ASSERT_TRUE(got->type);
  EXPECT_TRUE(std::holds_alternative<chat::groupchat>(*got->type));
  EXPECT_EQ(chevron::to_xml(*got) | std::ranges::to<std::string>(),
            R"(<note xmlns="urn:example:client" type="groupchat"><body>x</body></note>)");
  const auto none = chevron::read<chat::typed_note>(
      std::string_view(R"(<note xmlns="urn:example:client"/>)") | chevron::events);
  ASSERT_TRUE(none.has_value());
  EXPECT_FALSE(none->type);
  EXPECT_FALSE(chevron::read<chat::typed_note>(
      std::string_view(R"(<note xmlns="urn:example:client" type="bogus"/>)") | chevron::events));
}

}  // namespace

namespace kinds {

struct chat {
  std::optional<std::string> body;
};
constexpr auto xml_schema(chevron::type<chat>) {
  return chevron::schema<chat>().name("urn:example:client", "message").when<"type">("chat");
}

struct normal {
  std::optional<std::string> body;
};
constexpr auto xml_schema(chevron::type<normal>) {
  return chevron::schema<normal>().name("urn:example:client", "message").when<"type">("normal", chevron::or_absent);
}

}  // namespace kinds

namespace {

TEST(Read, TellingAttribute) {
  const auto pick = [](std::string_view text) {
    chevron::parser source;
    source.feed(text);
    return chevron::read_one_of<kinds::chat, kinds::normal>(source);
  };
  const auto chat = pick(R"(<message xmlns="urn:example:client" type="chat"><body>a</body></message>)");
  ASSERT_TRUE(chat.has_value());
  EXPECT_EQ(chat->index(), 0u);
  const auto plain = pick(R"(<message xmlns="urn:example:client"><body>b</body></message>)");
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(plain->index(), 1u);
  const auto named = pick(R"(<message xmlns="urn:example:client" type="normal"/>)");
  ASSERT_TRUE(named.has_value());
  EXPECT_EQ(named->index(), 1u);
  EXPECT_FALSE(pick(R"(<message xmlns="urn:example:client" type="headline"/>)").has_value());
  // Written with its attribute; the one absent means, without.
  EXPECT_EQ(chevron::to_xml(kinds::chat{"x"}) | std::ranges::to<std::string>(),
            R"(<message xmlns="urn:example:client" type="chat"><body>x</body></message>)");
  std::string eager;
  auto out = std::back_inserter(eager);
  chevron::write(out, kinds::chat{"x"});
  EXPECT_EQ(eager, R"(<message xmlns="urn:example:client" type="chat"><body>x</body></message>)");
  EXPECT_EQ(chevron::to_xml(kinds::normal{"y"}) | std::ranges::to<std::string>(),
            R"(<message xmlns="urn:example:client"><body>y</body></message>)");
}

}  // namespace

namespace tagged_test {
struct version {
  std::string name;
  std::string version;
};
constexpr auto xml_schema(chevron::type<version>) {
  return chevron::schema<version>().name("jabber:iq:version", "query");
}
struct delay {
  std::string stamp;
};
constexpr auto xml_schema(chevron::type<delay>) {
  using namespace chevron::members;
  return chevron::schema<delay>().name("urn:xmpp:delay", "delay").member<"stamp">(attribute());
}
struct result {
  std::optional<std::string> id;
  chevron::tagged<version, delay, chevron::any> payload;
};
constexpr auto xml_schema(chevron::type<result>) {
  using namespace chevron::members;
  return chevron::schema<result>().name("jabber:client", "iq").member<"id">(attribute());
}
struct note {
  std::string body;
  std::vector<chevron::tagged<delay, chevron::any>> extensions;
};
constexpr auto xml_schema(chevron::type<note>) { return chevron::schema<note>().name("jabber:client", "message"); }
}  // namespace tagged_test

// A child chosen by its name and read straight into its type; chevron::any
// only for what no alternative names; written back as it was.
TEST(Read, Tagged) {
  using namespace tagged_test;
  const auto answer = chevron::read<result>(
      events("<iq xmlns='jabber:client' id='v1'><query xmlns='jabber:iq:version'><name>ejabberd</name>"
             "<version>26.7.0</version></query></iq>"));
  ASSERT_TRUE(answer.has_value());
  ASSERT_TRUE(answer->payload.is<version>());
  EXPECT_EQ(answer->payload.as<version>().version, "26.7.0");

  const auto other = chevron::read<result>(events("<iq xmlns='jabber:client' id='p'><ping xmlns='urn:xmpp:ping'/></iq>"));
  ASSERT_TRUE(other.has_value());
  ASSERT_TRUE(other->payload.is<chevron::any>());
  EXPECT_EQ(other->payload.as<chevron::any>().local, "ping");

  const auto missing = chevron::read<result>(events("<iq xmlns='jabber:client' id='e'/>"));
  EXPECT_FALSE(missing.has_value());

  const auto message = chevron::read<note>(
      events("<message xmlns='jabber:client'><body>hi</body><delay xmlns='urn:xmpp:delay' stamp='2002-09-10T23:08:25Z'/>"
             "<active xmlns='http://jabber.org/protocol/chatstates'/></message>"));
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->body, "hi");
  ASSERT_EQ(message->extensions.size(), 2u);
  EXPECT_EQ(message->extensions[0].as<delay>().stamp, "2002-09-10T23:08:25Z");
  ASSERT_TRUE(message->extensions[1].is<chevron::any>());
  EXPECT_EQ(message->extensions[1].as<chevron::any>().local, "active");

  std::string eager;
  chevron::write(std::back_inserter(eager), *message);
  const std::string lazy = chevron::to_xml(*message) | std::ranges::to<std::string>();
  EXPECT_EQ(eager, lazy);
  EXPECT_NE(lazy.find("<delay xmlns=\"urn:xmpp:delay\" stamp=\"2002-09-10T23:08:25Z\"/>"), std::string::npos) << lazy;
  const auto back = chevron::read<note>(std::string_view(lazy) | chevron::events);
  ASSERT_TRUE(back.has_value()) << lazy;
  ASSERT_EQ(back->extensions.size(), 2u);
  EXPECT_EQ(back->extensions[0].as<delay>().stamp, "2002-09-10T23:08:25Z");
  EXPECT_EQ(back->extensions[1].as<chevron::any>().uri, "http://jabber.org/protocol/chatstates");

  std::string answer_written;
  chevron::write(std::back_inserter(answer_written), *answer);
  const auto answer_back = chevron::read<result>(std::string_view(answer_written) | chevron::events);
  ASSERT_TRUE(answer_back.has_value()) << answer_written;
  EXPECT_EQ(answer_back->payload.as<version>().name, "ejabberd");
}
