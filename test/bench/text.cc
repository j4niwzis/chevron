// Character data read through the events view: a stanza with a long body,
// ASCII and Cyrillic, many times over; megabytes a second.
import std;
import chevron;

int main() {
  std::string ascii = "<message xmlns='jabber:client' to='romeo@example.net'><body>";
  for (int at = 0; at != 400; ++at) ascii += "This is a line of a long message body. ";
  ascii += "</body></message>";
  std::string cyrillic = "<message xmlns='jabber:client' to='romeo@example.net'><body>";
  for (int at = 0; at != 400; ++at) cyrillic += "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82, \xd0\xbc\xd0\xb8\xd1\x80! ";
  cyrillic += "</body></message>";
  for (const auto& [name, text] : {std::pair{"ascii body", &ascii}, std::pair{"cyrillic body", &cyrillic}}) {
    double best = 1e300;
    for (int round = 0; round != 5; ++round) {
      const auto start = std::chrono::steady_clock::now();
      std::size_t seen = 0;
      for (int at = 0; at != 2000; ++at) {
        for (auto&& one : std::string_view(*text) | chevron::events) {
          if (!one) std::abort();
          ++seen;
        }
      }
      best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
      if (seen == 0) std::abort();
    }
    std::println("{:<14} {:>8.0f} MB/s", name, double(text->size()) * 2000 / 1e6 / best);
  }
}
