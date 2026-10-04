// Allocation regressions in serialization, measured without output growth or
// a test framework allocating inside the measured region.
import std;
import chevron;

namespace {
bool measuring = false;
std::size_t allocations = 0;
}

void* operator new(std::size_t size) {
  if (measuring) ++allocations;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace allocation_test {
struct document {
  std::string id;
  int sequence;
  std::string body;
  chevron::kept_attributes attributes;
  std::vector<chevron::any> extensions;
};
constexpr auto xml_schema(chevron::type<document>) {
  using namespace chevron::members;
  return chevron::schema<document>().name("urn:test", "doc")
      .members(attribute(), attribute(), child_text(), unknown_attributes(), unknown_children());
}
}

int main() {
  allocation_test::document value{std::string(65536, 'a'), 42, std::string(65536, '&'), {}, {}};
  value.attributes.push_back({{"http://www.w3.org/XML/1998/namespace", "lang"}, "en"});
  value.attributes.push_back({{"urn:other", "other"}, std::string(65536, 'b')});
  value.extensions.push_back({"urn:extension", "x", {}, {{std::string(65536, 'c')}}});
  std::string eager, lazy, owned;
  for (auto* output : {&eager, &lazy, &owned}) output->reserve(1 << 20);
  auto owned_value = value;

  measuring = true;
  chevron::write(std::back_inserter(eager), value);
  const auto after_eager = allocations;
  auto borrowed = chevron::to_xml(value);
  for (auto chunk : borrowed.chunks()) lazy += chunk;
  const auto after_borrowed = allocations;
  auto view = chevron::to_xml(std::move(owned_value));
  const auto after_ownership = allocations;
  auto moved = std::move(view);
  const auto after_move = allocations;
  for (auto chunk : moved.chunks()) owned += chunk;
  measuring = false;

  if (allocations != 0 || eager != lazy || eager != owned) {
    std::cerr << "allocations after eager, borrowed, ownership, move, owned: " << after_eager << ", " << after_borrowed << ", " << after_ownership << ", " << after_move << ", " << allocations << '\n';
    return 1;
  }
}
