// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT
//
// Bundled from xml-query @ a428a83 until it becomes its own library.

#pragma once

#include <expat.h>

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace xq {

enum class ParseStatus {
  Ok,
  Error,
};

constexpr std::string_view local_name(std::string_view qname) noexcept {
  // Namespaces may contain colons; keep the part after the last colon.
  auto pos = qname.rfind(':');
  return pos == std::string_view::npos ? qname : qname.substr(pos + 1);
}

// ----------------------------------------------------------------------------
// Concepts for clearer compile-time diagnostics
// ----------------------------------------------------------------------------

template <typename C>
concept AppendableContainer =
    requires(C& c, typename C::value_type v) { c.push_back(std::move(v)); };

template <typename T>
concept StringLike = std::is_same_v<T, std::string>;

template <typename T>
concept StringOrArithmetic =
    std::is_same_v<T, std::string> || std::is_arithmetic_v<T>;

namespace detail {

template <StringLike T>
void append_to(T& dst, std::string_view chunk) {
  dst.append(chunk);
}

template <StringOrArithmetic T>
bool assign_to(T& dst, std::string_view s) {
  if constexpr (std::is_same_v<T, std::string>) {
    dst.assign(s);
    return true;
  } else {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), dst);
    return ec == std::errc{} && ptr == s.data() + s.size();
  }
}

// A sink fails when a nested document it parses is malformed. Sinks without
// a failure of their own (writers, Invoke) needn't declare failed().
template <typename S>
bool failed(const S& sink) {
  if constexpr (requires { sink.failed(); }) {
    return sink.failed();
  } else {
    return false;
  }
}

// What a builder makes: sinks are built in place by calling a builder, a
// callable returning the sink by value, so that no sink is ever moved.
template <typename Make>
using Built = std::invoke_result_t<const Make&>;

template <std::size_t I, typename T>
struct TupleLeaf {
  T value;
};

template <typename Indices, typename... Ts>
class InPlaceTupleImpl;

// A tuple whose elements are built in place from builders, which std::tuple
// can't do: it always moves its elements in.
template <std::size_t... Is, typename... Ts>
class InPlaceTupleImpl<std::index_sequence<Is...>, Ts...>
    : TupleLeaf<Is, Ts>... {
 public:
  template <typename... Makes>
    requires(std::is_invocable_v<const Makes&> && ...)
  explicit InPlaceTupleImpl(const Makes&... makes)
      : TupleLeaf<Is, Ts>{makes()}... {}

  template <typename F>
  decltype(auto) apply(F&& f) {
    return std::forward<F>(f)(static_cast<TupleLeaf<Is, Ts>&>(*this).value...);
  }

  template <typename F>
  decltype(auto) apply(F&& f) const {
    return std::forward<F>(f)(
        static_cast<const TupleLeaf<Is, Ts>&>(*this).value...);
  }
};

template <typename... Ts>
using InPlaceTuple = InPlaceTupleImpl<std::index_sequence_for<Ts...>, Ts...>;

template <typename Tuple>
bool any_failed(const Tuple& sinks) {
  return sinks.apply([](const auto&... s) { return (failed(s) || ...); });
}

}  // namespace detail

// ----------------------------------------------------------------------------
// Element matchers, for Select(...).with(...)
// ----------------------------------------------------------------------------

/// An element's attributes as Expat hands them: name, value, ..., null.
class Attributes {
 public:
  explicit Attributes(const char** attrs) : attrs_(attrs) {}

  [[nodiscard]] std::optional<std::string_view> get(
      std::string_view name) const {
    for (const char** a = attrs_; *a != nullptr; a += 2) {
      if (local_name(a[0]) == name) {
        return std::string_view(a[1]);
      }
    }
    return std::nullopt;
  }

 private:
  const char** attrs_;
};

/// Matches every element: Select() without with().
struct MatchAll {
  bool operator()(const Attributes& /*attrs*/) const { return true; }
};

/// Matches elements whose attribute `name` is `value`: Attr(name, value).
struct AttrEquals {
  std::string_view name;
  std::string_view value;
  bool operator()(const Attributes& attrs) const {
    return attrs.get(name) == value;
  }
};

/// Matches by custom logic: Predicate([](const Attributes&) { ... }).
template <typename Fn>
struct PredicateMatcher {
  Fn fn;
  bool operator()(const Attributes& attrs) const { return fn(attrs); }
};

/// Both must match: what chained with() calls build.
template <typename A, typename B>
struct AllOf {
  A a;
  B b;
  bool operator()(const Attributes& attrs) const {
    return a(attrs) && b(attrs);
  }
};

struct NullSink {
  void receive(const auto&) {}
  void done() {}
  void reset_target(auto&) {}
};

namespace detail {

struct MakeNullSink {
  NullSink operator()() const { return {}; }
};

}  // namespace detail

// ----------------------------------------------------------------------------
// SAX sinks
// ----------------------------------------------------------------------------

template <typename Downstream, typename Matcher = MatchAll>
class SelectSink {
  std::string_view name_;
  Matcher matcher_;
  Downstream downstream_;
  int depth_ = 0;

 public:
  template <typename Make>
  SelectSink(std::string_view name, Matcher matcher, const Make& make)
      : name_(name), matcher_(std::move(matcher)), downstream_(make()) {}

  void on_start(std::string_view name, const char** attrs) {
    if (depth_ == 0) {
      if (name == name_ && matcher_(Attributes(attrs))) {
        depth_ = 1;
        downstream_.on_start(name, attrs);
      }
    } else {
      ++depth_;
      downstream_.on_start(name, attrs);
    }
  }

  void on_text(std::string_view chunk) {
    if (depth_ > 0) {
      downstream_.on_text(chunk);
    }
  }

  void on_end(std::string_view name) {
    if (depth_ > 0) {
      downstream_.on_end(name);
      --depth_;
    }
  }

  void reset_target(auto& obj) { downstream_.reset_target(obj); }
  [[nodiscard]] bool failed() const { return detail::failed(downstream_); }
};

template <typename Downstream>
class TextSink {
  Downstream downstream_;
  int depth_ = 0;

 public:
  template <typename Make>
  explicit TextSink(const Make& make) : downstream_(make()) {}

  void on_start(std::string_view /*name*/, const char** /*attrs*/) {
    if (depth_ == 0) {
      depth_ = 1;
    } else {
      ++depth_;
    }
  }

  void on_text(std::string_view chunk) {
    if (depth_ > 0) {
      downstream_.receive(chunk);
    }
  }

  void on_end(std::string_view /*name*/) {
    if (depth_ > 0) {
      --depth_;
      if (depth_ == 0) {
        downstream_.done();
      }
    }
  }

  void reset_target(auto& obj) { downstream_.reset_target(obj); }
  [[nodiscard]] bool failed() const { return detail::failed(downstream_); }
};

template <typename Downstream>
class AttrSink {
  std::string_view name_;
  Downstream downstream_;
  int depth_ = 0;
  bool emitted_ = false;

 public:
  template <typename Make>
  AttrSink(std::string_view name, const Make& make)
      : name_(name), downstream_(make()) {}

  void on_start(std::string_view /*name*/, const char** attrs) {
    if (depth_ == 0) {
      depth_ = 1;
      emitted_ = false;
      for (const char** a = attrs; *a; a += 2) {
        if (a[0] == name_) {
          downstream_.receive(std::string_view(a[1]));
          emitted_ = true;
          break;
        }
      }
    } else {
      ++depth_;
    }
  }

  void on_text(std::string_view /*chunk*/) {}

  void on_end(std::string_view /*name*/) {
    if (depth_ > 0) {
      --depth_;
      if (depth_ == 0 && emitted_) {
        downstream_.done();
      }
    }
  }

  void reset_target(auto& obj) { downstream_.reset_target(obj); }
  [[nodiscard]] bool failed() const { return detail::failed(downstream_); }
};

template <typename Downstream>
class ParseXmlSink {
  Downstream downstream_;
  XML_Parser parser_ = nullptr;
  // Sticky: once a nested document is malformed, the whole parse has failed.
  bool failed_ = false;

  static void start_cb(void* user_data, const XML_Char* name,
                       const XML_Char** attrs) {
    auto* self = static_cast<ParseXmlSink*>(user_data);
    self->downstream_.on_start(local_name(name), attrs);
  }

  static void text_cb(void* user_data, const XML_Char* s, int len) {
    auto* self = static_cast<ParseXmlSink*>(user_data);
    self->downstream_.on_text(std::string_view{reinterpret_cast<const char*>(s),
                                               static_cast<size_t>(len)});
  }

  static void end_cb(void* user_data, const XML_Char* name) {
    auto* self = static_cast<ParseXmlSink*>(user_data);
    self->downstream_.on_end(local_name(name));
  }

 public:
  // Built in place, never moved: Expat holds `this` as its user data.
  template <typename Make>
  explicit ParseXmlSink(const Make& make)
      : downstream_(make()), parser_(XML_ParserCreate(nullptr)) {
    XML_SetUserData(parser_, this);
    XML_SetStartElementHandler(parser_, start_cb);
    XML_SetCharacterDataHandler(parser_, text_cb);
    XML_SetEndElementHandler(parser_, end_cb);
  }

  ~ParseXmlSink() {
    if (parser_) {
      XML_ParserFree(parser_);
    }
  }

  ParseXmlSink(const ParseXmlSink&) = delete;
  ParseXmlSink& operator=(const ParseXmlSink&) = delete;

  void receive(std::string_view chunk) {
    if (failed_) return;
    if (XML_Parse(parser_, chunk.data(), static_cast<int>(chunk.size()),
                  XML_FALSE) == XML_STATUS_ERROR) {
      failed_ = true;
    }
  }

  void done() {
    if (!failed_ &&
        XML_Parse(parser_, nullptr, 0, XML_TRUE) == XML_STATUS_ERROR) {
      failed_ = true;
    }
    XML_ParserReset(parser_, nullptr);
    // Reset preserves handlers but clears user data on some Expat versions.
    XML_SetUserData(parser_, this);
    XML_SetStartElementHandler(parser_, start_cb);
    XML_SetCharacterDataHandler(parser_, text_cb);
    XML_SetEndElementHandler(parser_, end_cb);
  }

  void reset_target(auto& obj) { downstream_.reset_target(obj); }
  [[nodiscard]] bool failed() const {
    return failed_ || detail::failed(downstream_);
  }
};

template <typename Fn>
class InvokeSink {
  Fn fn_;

 public:
  explicit InvokeSink(Fn fn) : fn_(std::move(fn)) {}

  void receive(const auto& item) { fn_(item); }
  void done() {}
  void reset_target(auto&) {}
};

template <typename T, typename Downstream, typename... Collectors>
class ConstructSink {
  Downstream downstream_;
  detail::InPlaceTuple<Collectors...> collectors_;
  std::optional<T> storage_;
  int depth_ = 0;

 public:
  template <typename Make, typename... MakeCollectors>
  explicit ConstructSink(const Make& make,
                         const MakeCollectors&... make_collectors)
      : downstream_(make()), collectors_(make_collectors...) {}

  void on_start(std::string_view name, const char** attrs) {
    if (depth_ == 0) {
      storage_.emplace();
      ++depth_;
      collectors_.apply(
          [&](auto&... c) { (c.bind_attrs(*storage_, attrs), ...); });
      collectors_.apply([&](auto&... c) { (c.reset(*storage_), ...); });
    } else {
      ++depth_;
      collectors_.apply(
          [&](auto&... c) { (c.on_start(name, attrs, *storage_), ...); });
    }
  }

  void on_text(std::string_view chunk) {
    if (depth_ > 0) {
      collectors_.apply(
          [&](auto&... c) { (c.on_text(chunk, *storage_), ...); });
    }
  }

  void on_end(std::string_view name) {
    if (depth_ > 0) {
      collectors_.apply([&](auto&... c) { (c.on_end(name, *storage_), ...); });
      --depth_;
      if (depth_ == 0) {
        downstream_.receive(*storage_);
        storage_.reset();
      }
    }
  }

  void reset_target(auto& obj) {
    collectors_.apply([&](auto&... c) { (c.reset(obj), ...); });
    downstream_.reset_target(obj);
  }
  [[nodiscard]] bool failed() const {
    return detail::any_failed(collectors_) || detail::failed(downstream_);
  }
};

template <typename Downstream, typename... Sinks>
class FanoutSink {
  Downstream downstream_;
  detail::InPlaceTuple<Sinks...> sinks_;

 public:
  template <typename Make, typename... MakeSinks>
  explicit FanoutSink(const Make& make, const MakeSinks&... make_sinks)
      : downstream_(make()), sinks_(make_sinks...) {}

  void on_start(std::string_view name, const char** attrs) {
    sinks_.apply([&](auto&... s) { (s.on_start(name, attrs), ...); });
  }

  void on_text(std::string_view chunk) {
    sinks_.apply([&](auto&... s) { (s.on_text(chunk), ...); });
  }

  void on_end(std::string_view name) {
    sinks_.apply([&](auto&... s) { (s.on_end(name), ...); });
  }

  void receive(const auto& item) { downstream_.receive(item); }
  void done() { downstream_.done(); }
  void reset_target(auto& obj) {
    sinks_.apply([&](auto&... s) { (s.reset_target(obj), ...); });
    downstream_.reset_target(obj);
  }
  [[nodiscard]] bool failed() const {
    return detail::any_failed(sinks_) || detail::failed(downstream_);
  }
};

// ----------------------------------------------------------------------------
// Factories
//
// build(make) returns the factory's sink by value, with its downstream built
// in place by `make`. Every sink is built straight into its final storage.
// ----------------------------------------------------------------------------

template <typename Matcher = MatchAll>
struct SelectFactory {
  std::string_view name;
  Matcher matcher{};

  /// Narrows the selection to elements `m` matches, e.g. Attr("channel",
  /// "Master") or Predicate(...). Chained calls must all match.
  template <typename M>
  [[nodiscard]] auto with(M m) const {
    if constexpr (std::is_same_v<Matcher, MatchAll>) {
      return SelectFactory<M>{name, std::move(m)};
    } else {
      return SelectFactory<AllOf<Matcher, M>>{name, {matcher, std::move(m)}};
    }
  }

  template <typename Make>
  [[nodiscard]] auto build(const Make& make) const {
    return SelectSink<detail::Built<Make>, Matcher>(name, matcher, make);
  }
};

struct TextFactory {
  template <typename Make>
  [[nodiscard]] auto build(const Make& make) const {
    return TextSink<detail::Built<Make>>(make);
  }
};

// Forward declaration for the bind expression AttrExpr produces.
template <typename T, typename Member>
class AttrBind;

class AttrExpr {
  std::string_view name_;

 public:
  AttrExpr(std::string_view name) : name_(name) {}

  template <typename Make>
  [[nodiscard]] auto build(const Make& make) const {
    return AttrSink<detail::Built<Make>>(name_, make);
  }

  template <typename T, typename Member>
  [[nodiscard]] auto make_collector(Member T::* member) const {
    return AttrBind<T, Member>(member, name_);
  }

  [[nodiscard]] std::string_view name() const { return name_; }
};

struct ParseXmlFactory {
  template <typename Make>
  [[nodiscard]] auto build(const Make& make) const {
    return ParseXmlSink<detail::Built<Make>>(make);
  }
};

template <typename Fn>
struct InvokeFactory {
  Fn fn;

  template <typename Make>
  [[nodiscard]] auto build(const Make& /*make*/) const {
    return InvokeSink<Fn>(fn);
  }
};

template <typename T, typename... Binds>
struct ConstructFactory {
  std::tuple<Binds...> binds;

  template <typename Make>
  [[nodiscard]] auto build(const Make& make) const {
    return std::apply(
        [&](const auto&... b) {
          return ConstructSink<T, detail::Built<Make>,
                               decltype(b.make_collector())...>(
              make, [&b] { return b.make_collector(); }...);
        },
        binds);
  }
};

template <typename... Factories>
struct FanoutFactory {
  std::tuple<Factories...> factories;

  // Each branch, and the fanout itself, gets a downstream of its own.
  template <typename Make>
  [[nodiscard]] auto build(const Make& make) const {
    return std::apply(
        [&](const auto&... f) {
          return FanoutSink<detail::Built<Make>, decltype(f.build(make))...>(
              make, [&f, &make] { return f.build(make); }...);
        },
        factories);
  }
};

template <typename A, typename B>
struct PipeFactory {
  A a;
  B b;

  template <typename Make>
  [[nodiscard]] auto build(const Make& make) const {
    return a.build([&] { return b.build(make); });
  }
};

// ----------------------------------------------------------------------------
// Bind expressions and collectors
// ----------------------------------------------------------------------------

template <typename T, typename Member>
class AttrBind {
  Member T::* member_;
  std::string_view name_;

 public:
  AttrBind(Member T::* member, std::string_view name)
      : member_(member), name_(name) {}

  void bind_attrs(T& obj, const char** attrs) const {
    for (const char** a = attrs; *a; a += 2) {
      if (a[0] == name_) {
        detail::assign_to(obj.*member_, std::string_view(a[1]));
        break;
      }
    }
  }

  void reset(T&) {}
  void on_start(std::string_view, const char**, T&) {}
  void on_text(std::string_view, T&) {}
  void on_end(std::string_view, T&) {}
};

template <typename T, typename Member>
  requires StringOrArithmetic<Member>
class FirstWriter {
  T* obj_ = nullptr;
  Member T::* member_;
  bool captured_ = false;

 public:
  explicit FirstWriter(Member T::* member) : member_(member) {}

  void reset_target(T& obj) {
    obj_ = &obj;
    captured_ = false;
  }

  void receive(std::string_view chunk) {
    if (!captured_ && obj_) {
      detail::append_to(obj_->*member_, chunk);
    }
  }

  void done() { captured_ = true; }
};

template <typename T, typename Member>
  requires AppendableContainer<Member> &&
           StringOrArithmetic<typename Member::value_type>
class CollectWriter {
  T* obj_ = nullptr;
  Member T::* member_;
  bool in_element_ = false;

 public:
  explicit CollectWriter(Member T::* member) : member_(member) {}

  void reset_target(T& obj) {
    obj_ = &obj;
    in_element_ = false;
  }

  void receive(std::string_view chunk) {
    if (!obj_) return;
    if (!in_element_) {
      (obj_->*member_).emplace_back();
      in_element_ = true;
    }
    detail::append_to((obj_->*member_).back(), chunk);
  }

  void done() {
    if (!obj_) return;
    if (!in_element_) {
      (obj_->*member_).emplace_back();
    }
    in_element_ = false;
  }
};

// First(...) and Collect(...) in a Bind: a pipeline ending in the member's
// writer, fed the elements inside the constructed one.
template <typename Pipeline>
class PipelineCollector {
  Pipeline pipeline_;

 public:
  template <typename Make>
  explicit PipelineCollector(const Make& make) : pipeline_(make()) {}

  void bind_attrs(auto&, const char**) {}
  void reset(auto& obj) { pipeline_.reset_target(obj); }
  void on_start(std::string_view name, const char** attrs, auto&) {
    pipeline_.on_start(name, attrs);
  }
  void on_text(std::string_view chunk, auto&) { pipeline_.on_text(chunk); }
  void on_end(std::string_view name, auto&) { pipeline_.on_end(name); }
  [[nodiscard]] bool failed() const { return detail::failed(pipeline_); }
};

namespace detail {

template <typename PipelineFactory, typename MakeWriter>
auto make_pipeline_collector(const PipelineFactory& factory,
                             const MakeWriter& make_writer) {
  return PipelineCollector<decltype(factory.build(make_writer))>(
      [&] { return factory.build(make_writer); });
}

}  // namespace detail

template <typename T, typename Member, typename Expr>
class BindSpec {
  Member T::* member_;
  Expr expr_;

 public:
  BindSpec(Member T::* member, Expr expr)
      : member_(member), expr_(std::move(expr)) {}

  [[nodiscard]] auto make_collector() const {
    return expr_.template make_collector<T, Member>(member_);
  }
};

template <typename PipelineFactory>
class FirstExpr {
  PipelineFactory factory_;

 public:
  FirstExpr(PipelineFactory factory) : factory_(std::move(factory)) {}

  template <typename T, typename Member>
  [[nodiscard]] auto make_collector(Member T::* member) const {
    return detail::make_pipeline_collector(
        factory_, [member] { return FirstWriter<T, Member>(member); });
  }
};

template <typename PipelineFactory>
class CollectExpr {
  PipelineFactory factory_;

 public:
  CollectExpr(PipelineFactory factory) : factory_(std::move(factory)) {}

  template <typename T, typename Member>
  [[nodiscard]] auto make_collector(Member T::* member) const {
    return detail::make_pipeline_collector(
        factory_, [member] { return CollectWriter<T, Member>(member); });
  }
};

// ----------------------------------------------------------------------------
// User-facing helpers
// ----------------------------------------------------------------------------

inline auto Select(std::string_view name) {
  return SelectFactory<>{.name = name};
}
inline auto Text() { return TextFactory{}; }
inline auto Attr(std::string_view name) { return AttrExpr{name}; }
/// A matcher, for Select(...).with(...): attribute `name` equals `value`.
inline auto Attr(std::string_view name, std::string_view value) {
  return AttrEquals{.name = name, .value = value};
}
template <typename Fn>
auto Predicate(Fn fn) {
  return PredicateMatcher<Fn>{std::move(fn)};
}
inline auto ParseXml() { return ParseXmlFactory{}; }

template <typename Fn>
auto Invoke(Fn fn) {
  return InvokeFactory<Fn>{std::move(fn)};
}

template <typename T, typename... Binds>
auto Construct(Binds... binds) {
  return ConstructFactory<T, Binds...>{std::make_tuple(std::move(binds)...)};
}

template <typename... Factories>
auto Fanout(Factories... factories) {
  // One tuple member: the branches go in through make_tuple, not braces.
  return FanoutFactory<Factories...>{std::make_tuple(std::move(factories)...)};
}

template <typename Member, typename T, typename Expr>
auto Bind(Member T::* member, Expr expr) {
  return BindSpec<T, Member, Expr>{member, std::move(expr)};
}

template <typename PipelineFactory>
auto First(PipelineFactory factory) {
  return FirstExpr<PipelineFactory>{std::move(factory)};
}

template <typename PipelineFactory>
auto Collect(PipelineFactory factory) {
  return CollectExpr<PipelineFactory>{std::move(factory)};
}

// ----------------------------------------------------------------------------
// Pipeline composition
// ----------------------------------------------------------------------------

template <typename A, typename B>
auto operator|(A a, B b) {
  return PipeFactory<A, B>{std::move(a), std::move(b)};
}

// ----------------------------------------------------------------------------
// Top-level parser
// ----------------------------------------------------------------------------

template <typename Query>
class XmlParser {
  using Sink = decltype(std::declval<const Query&>().build(
      std::declval<detail::MakeNullSink>()));
  Sink sink_;
  XML_Parser parser_ = nullptr;

  static void start_cb(void* user_data, const XML_Char* name,
                       const XML_Char** attrs) {
    auto* self = static_cast<XmlParser*>(user_data);
    self->sink_.on_start(local_name(name), attrs);
  }

  static void text_cb(void* user_data, const XML_Char* s, int len) {
    auto* self = static_cast<XmlParser*>(user_data);
    self->sink_.on_text(std::string_view{reinterpret_cast<const char*>(s),
                                         static_cast<size_t>(len)});
  }

  static void end_cb(void* user_data, const XML_Char* name) {
    auto* self = static_cast<XmlParser*>(user_data);
    self->sink_.on_end(local_name(name));
  }

 public:
  // Builds the query's sinks in place. Never moved: Expat holds `this` as its
  // user data, so construct it where it lives (e.g. optional::emplace(query)).
  explicit XmlParser(const Query& query)
      : sink_(query.build(detail::MakeNullSink{})),
        parser_(XML_ParserCreate(nullptr)) {
    XML_SetUserData(parser_, this);
    XML_SetStartElementHandler(parser_, start_cb);
    XML_SetCharacterDataHandler(parser_, text_cb);
    XML_SetEndElementHandler(parser_, end_cb);
  }

  ~XmlParser() {
    if (parser_) {
      XML_ParserFree(parser_);
    }
  }

  XmlParser(const XmlParser&) = delete;
  XmlParser& operator=(const XmlParser&) = delete;

  ParseStatus feed(std::string_view chunk, bool is_final = false) {
    if (XML_Parse(parser_, chunk.data(), static_cast<int>(chunk.size()),
                  is_final ? XML_TRUE : XML_FALSE) == XML_STATUS_ERROR) {
      return ParseStatus::Error;
    }
    return detail::failed(sink_) ? ParseStatus::Error : ParseStatus::Ok;
  }

  ParseStatus finish() { return feed({}, true); }
};

}  // namespace xq
