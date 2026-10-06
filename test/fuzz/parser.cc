// The parser against itself: the events of the input fed whole, and fed in
// pieces cut where the input's own bytes say, must be the same -- the same
// events, and the same error where there is one.
import std;
import splice;
import chevron;

namespace {

std::string shown(const chevron::event& one) {
  if (const auto* start = spl::get_if<chevron::start_element>(&one)) {
    std::string out = "S{" + std::string(start->name.uri) + "}" + std::string(start->name.local);
    for (const auto& attribute : start->attributes)
      out += " {" + std::string(attribute.name.uri) + "}" + std::string(attribute.name.local) + "=" +
             std::string(attribute.value);
    return out;
  }
  if (spl::holds_alternative<chevron::end_element>(one))
    return "E";
  return "T" + std::string(spl::get<chevron::text>(one).content);
}

// The events, texts run together as a reader sees them, and how it ended.
std::vector<std::string> events(const std::vector<std::string_view>& pieces) {
  chevron::parser p;
  std::vector<std::string> out;
  std::string text;
  const auto flush = [&] {
    if (!text.empty())
      out.push_back("T" + text);
    text.clear();
  };
  const auto drain = [&] {
    for (;;) {
      auto next = p.next();
      if (!next) {
        flush();
        out.push_back("error " + std::to_string(static_cast<int>(next.error().code)));
        return false;
      }
      if (!*next)
        return true;
      const std::string one = shown(**next);
      if (one[0] == 'T') {
        text += one.substr(1);
      } else {
        flush();
        out.push_back(one);
      }
    }
  };
  for (std::string_view piece : pieces) {
    p.feed(piece);
    if (!drain())
      return out;
  }
  p.finish();
  if (drain())
    flush();
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // The fuzzer's bytes as text, a byte at a time by value: no view of one
  // type as another.
  const std::string text = std::span(data, size) | std::views::transform([](std::uint8_t b) { return std::bit_cast<char>(b); }) |
                           std::ranges::to<std::string>();
  const std::string_view input(text);
  const auto whole = events({input});
  // Cut points from the input: after each byte whose low bits say so.
  std::vector<std::string_view> pieces;
  std::size_t from = 0;
  for (std::size_t at = 0; at < size; ++at)
    if ((data[at] & 7) == 3) {
      pieces.push_back(input.substr(from, at + 1 - from));
      from = at + 1;
    }
  pieces.push_back(input.substr(from));
  const auto split = events(pieces);
  if (whole != split)
    std::abort();
  return 0;
}
