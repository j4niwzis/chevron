// Events pulled from a range: text | chevron::events is a view of the events
// of an XML stream read from any input range of UTF-8 code units -- a string,
// a stream read once, a generator, pieces joined together -- as far as the
// next event needs and no further.
export module chevron.events;

import std;
import chevron.parser;

export namespace chevron {

// Each element is an event, or the error that ends the view. What an event
// refers to stays valid until the view goes on.
template <std::ranges::view V>
  requires std::ranges::input_range<V> && byte_unit<std::ranges::range_value_t<V>>
class event_view : public std::ranges::view_interface<event_view<V>> {
 public:
  class iterator {
   public:
    using value_type = std::expected<event, error>;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;

    constexpr explicit iterator(event_view* view) : view_(view) {}
    iterator(iterator&&) = default;
    iterator& operator=(iterator&&) = default;

    constexpr value_type operator*() const { return view_->current_; }
    constexpr iterator& operator++() {
      view_->advance();
      return *this;
    }
    constexpr void operator++(int) { ++*this; }
    friend constexpr bool operator==(const iterator& one, std::default_sentinel_t) {
      return one.view_->done_;
    }

   private:
    event_view* view_;
  };

  constexpr explicit event_view(V base, limits held = {}) : base_(std::move(base)), parser_(held) {}

  constexpr iterator begin() {
    at_.emplace(std::ranges::begin(base_));
    advance();
    return iterator(this);
  }
  constexpr std::default_sentinel_t end() const noexcept { return {}; }

 private:
  static constexpr std::size_t piece = 4096;

  constexpr void advance() {
    if (failed_) {
      done_ = true;
      return;
    }
    for (;;) {
      auto next = parser_.next();
      if (!next) {
        current_ = std::unexpected(next.error());
        failed_ = true;
        return;
      }
      if (*next) {
        current_ = **next;
        return;
      }
      if (read_all_) {
        done_ = true;
        return;
      }
      chunk_.clear();
      while (chunk_.size() < piece && *at_ != std::ranges::end(base_)) {
        chunk_.push_back(static_cast<char>(**at_));
        ++*at_;
      }
      parser_.feed(chunk_);
      if (*at_ == std::ranges::end(base_)) {
        parser_.finish();
        read_all_ = true;
      }
    }
  }

  V base_;
  parser parser_;
  std::optional<std::ranges::iterator_t<V>> at_;
  std::string chunk_;
  std::expected<event, error> current_;
  bool read_all_ = false;
  bool failed_ = false;
  bool done_ = false;
};

// text | events, or events(text). A string literal is best given as a
// string_view: as an array, its terminating NUL would be read as input.
struct events_fn : std::ranges::range_adaptor_closure<events_fn> {
  template <std::ranges::viewable_range Range>
    requires std::ranges::input_range<Range> && byte_unit<std::ranges::range_value_t<Range>>
  constexpr auto operator()(Range&& range) const {
    return event_view<std::views::all_t<Range>>(std::views::all(std::forward<Range>(range)));
  }
};
inline constexpr events_fn events{};

}  // namespace chevron
