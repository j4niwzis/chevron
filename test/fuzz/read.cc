// A type of every kind of member, read from the input; where it is read,
// written by both writers, which must agree, and read again, which must
// give what was written.
import std;
import splice;
import chevron;

namespace fz {

struct kind_a { static constexpr std::string_view xml_value = "a"; };
struct kind_b { static constexpr std::string_view xml_value = "b"; };

struct delay {
  std::string stamp;
};
constexpr auto xml_schema(chevron::type<delay>) {
  using namespace chevron::members;
  return chevron::schema<delay>().name("urn:d", "delay").member<"stamp">(attribute());
}

struct item {
  std::optional<std::string> jid;
  std::vector<std::string> group;
};
constexpr auto xml_schema(chevron::type<item>) {
  using namespace chevron::members;
  return chevron::schema<item>().name("urn:f", "item").member<"jid">(attribute());
}

struct doc {
  std::optional<std::string> id;
  std::optional<splice::variant<kind_a, kind_b>> type;
  chevron::kept_attributes rest;
  std::optional<std::string> body;
  std::optional<int> n;
  std::vector<item> items;
  std::vector<chevron::tagged<delay, chevron::any>> ext;
};
constexpr auto xml_schema(chevron::type<doc>) {
  using namespace chevron::members;
  return chevron::schema<doc>()
      .name("urn:f", "doc")
      .member<"id">(attribute())
      .member<"type">(attribute())
      .member<"rest">(unknown_attributes());
}

}  // namespace fz

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const auto read = chevron::read<fz::doc>(input | chevron::events);
  if (!read)
    return 0;
  std::string eager;
  chevron::write(std::back_inserter(eager), *read);
  const std::string lazy = chevron::to_xml(*read) | std::ranges::to<std::string>();
  if (eager != lazy)
    std::abort();
  const auto again = chevron::read<fz::doc>(std::string_view(eager) | chevron::events);
  if (!again)
    std::abort();
  std::string twice;
  chevron::write(std::back_inserter(twice), *again);
  if (twice != eager)
    std::abort();
  return 0;
}
