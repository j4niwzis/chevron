// SPDX-License-Identifier: AGPL-3.0-only
// Events pulled from a range: text | chevron::events is a view of the events
// of an XML stream read from any input range of UTF-8 code units -- a string,
// a stream read once, a socket, a generator -- unit by unit, as far as the
// next event needs and no further: to the '<' or '>' where one can end.
//
// Or from a range of chunks -- whatever arrived at once, a buffer read from a
// file -- each taken whole: pieces | chevron::events. chevron::chunked<N>
// makes chunks of up to N units of a range of units, for input where reading
// ahead costs nothing:
//
//   file | chevron::chunked<4096> | chevron::events
export module chevron.events;

import std;
import chevron.parser;

export namespace chevron {

// A range of chunks: each element a range of code units.
template <class Range>
concept chunk_range = std::ranges::input_range<Range> && std::ranges::input_range<std::ranges::range_reference_t<Range>> &&
                      byte_unit<std::ranges::range_value_t<std::ranges::range_reference_t<Range>>>;

template <class Range>
concept unit_range = std::ranges::input_range<Range> && byte_unit<std::ranges::range_value_t<Range>>;

// Each element is an event, or the error that ends the view. What an event
// refers to stays valid until the view goes on.
template <std::ranges::view V>
  requires unit_range<V> || chunk_range<V>
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
      pull();
    }
  }

  // More input for the parser. The end is asked about only where input is
  // needed: over a socket, asking is waiting for the peer.
  constexpr void pull() {
    if constexpr (unit_range<V>) {
      chunk_.clear();
      for (;;) {
        if (*at_ == std::ranges::end(base_)) {
          read_all_ = true;
          break;
        }
        const char unit = static_cast<char>(**at_);
        ++*at_;
        chunk_.push_back(unit);
        if (unit == '<' || unit == '>')
          break;
      }
      parser_.feed(chunk_);
    } else {
      if (*at_ == std::ranges::end(base_)) {
        read_all_ = true;
      } else {
        parser_.feed(**at_);
        ++*at_;
      }
    }
    if (read_all_)
      parser_.finish();
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

// text | events, or events(text); pieces | events. A string literal is best
// given as a string_view: as an array, its terminating NUL would be read as
// input.
struct events_fn : std::ranges::range_adaptor_closure<events_fn> {
  template <std::ranges::viewable_range Range>
    requires unit_range<Range> || chunk_range<Range>
  constexpr auto operator()(Range&& range) const {
    return event_view<std::views::all_t<Range>>(std::views::all(std::forward<Range>(range)));
  }
};
inline constexpr events_fn events{};

// Chunks of up to N code units of a range of them, each a std::string_view
// valid until the next: read N at a time, or to the end.
template <std::size_t N, std::ranges::view V>
  requires unit_range<V>
class chunk_view : public std::ranges::view_interface<chunk_view<N, V>> {
 public:
  class iterator {
   public:
    using value_type = std::string_view;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;

    constexpr explicit iterator(chunk_view* view) : view_(view) {}
    iterator(iterator&&) = default;
    iterator& operator=(iterator&&) = default;

    constexpr std::string_view operator*() const { return view_->chunk_; }
    constexpr iterator& operator++() {
      view_->read();
      return *this;
    }
    constexpr void operator++(int) { ++*this; }
    friend constexpr bool operator==(const iterator& one, std::default_sentinel_t) {
      return one.view_->chunk_.empty();
    }

   private:
    chunk_view* view_;
  };

  constexpr explicit chunk_view(V base) : base_(std::move(base)) {}

  constexpr iterator begin() {
    at_.emplace(std::ranges::begin(base_));
    read();
    return iterator(this);
  }
  constexpr std::default_sentinel_t end() const noexcept { return {}; }

 private:
  constexpr void read() {
    chunk_.clear();
    while (chunk_.size() < N && *at_ != std::ranges::end(base_)) {
      chunk_.push_back(static_cast<char>(**at_));
      ++*at_;
    }
  }

  V base_;
  std::optional<std::ranges::iterator_t<V>> at_;
  std::string chunk_;
};

template <std::size_t N>
  requires(N > 0)
struct chunked_fn : std::ranges::range_adaptor_closure<chunked_fn<N>> {
  template <std::ranges::viewable_range Range>
    requires unit_range<Range>
  constexpr auto operator()(Range&& range) const {
    return chunk_view<N, std::views::all_t<Range>>(std::views::all(std::forward<Range>(range)));
  }
};
template <std::size_t N>
inline constexpr chunked_fn<N> chunked{};

}  // namespace chevron
