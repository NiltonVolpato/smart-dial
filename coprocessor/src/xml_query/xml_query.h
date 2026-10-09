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

template <typename Tuple>
bool any_failed(const Tuple& sinks) {
  return std::apply([](const auto&... s) { return (failed(s) || ...); }, sinks);
}

}  // namespace detail

struct NullSink {
  void receive(const auto&) {}
  void done() {}
  void reset_target(auto&) {}
};

// ----------------------------------------------------------------------------
// SAX sinks
// ----------------------------------------------------------------------------

template <typename Downstream>
class SelectSink {
  std::string_view name_;
  Downstream downstream_;
  int depth_ = 0;

 public:
  SelectSink(std::string_view name, Downstream downstream)
      : name_(name), downstream_(std::move(downstream)) {}

  void on_start(std::string_view name, const char** attrs) {
    if (depth_ == 0) {
      if (name == name_) {
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
  explicit TextSink(Downstream downstream)
      : downstream_(std::move(downstream)) {}

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
  AttrSink(std::string_view name, Downstream downstream)
      : name_(name), downstream_(std::move(downstream)) {}

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

  void init_parser() {
    if (parser_) return;
    parser_ = XML_ParserCreate(nullptr);
    XML_SetUserData(parser_, this);
    XML_SetStartElementHandler(parser_, start_cb);
    XML_SetCharacterDataHandler(parser_, text_cb);
    XML_SetEndElementHandler(parser_, end_cb);
  }

 public:
  explicit ParseXmlSink(Downstream downstream)
      : downstream_(std::move(downstream)) {
    init_parser();
  }

  ~ParseXmlSink() {
    if (parser_) {
      XML_ParserFree(parser_);
    }
  }

  ParseXmlSink(ParseXmlSink&& other) noexcept
      : downstream_(std::move(other.downstream_)),
        parser_(other.parser_),
        failed_(other.failed_) {
    other.parser_ = nullptr;
    if (parser_) {
      XML_SetUserData(parser_, this);
    }
  }

  ParseXmlSink& operator=(ParseXmlSink&& other) noexcept {
    if (this != &other) {
      if (parser_) {
        XML_ParserFree(parser_);
      }
      downstream_ = std::move(other.downstream_);
      parser_ = other.parser_;
      failed_ = other.failed_;
      other.parser_ = nullptr;
      if (parser_) {
        XML_SetUserData(parser_, this);
      }
    }
    return *this;
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

template <typename T, typename Downstream, typename CollectorsTuple>
class ConstructSink {
  Downstream downstream_;
  CollectorsTuple collectors_;
  std::optional<T> storage_;
  int depth_ = 0;

 public:
  ConstructSink(Downstream downstream, CollectorsTuple collectors)
      : downstream_(std::move(downstream)),
        collectors_(std::move(collectors)) {}

  void on_start(std::string_view name, const char** attrs) {
    if (depth_ == 0) {
      storage_.emplace();
      ++depth_;
      std::apply([&](auto&... c) { (c.bind_attrs(*storage_, attrs), ...); },
                 collectors_);
      std::apply([&](auto&... c) { (c.reset(*storage_), ...); }, collectors_);
    } else {
      ++depth_;
      std::apply([&](auto&... c) { (c.on_start(name, attrs, *storage_), ...); },
                 collectors_);
    }
  }

  void on_text(std::string_view chunk) {
    if (depth_ > 0) {
      std::apply([&](auto&... c) { (c.on_text(chunk, *storage_), ...); },
                 collectors_);
    }
  }

  void on_end(std::string_view name) {
    if (depth_ > 0) {
      std::apply([&](auto&... c) { (c.on_end(name, *storage_), ...); },
                 collectors_);
      --depth_;
      if (depth_ == 0) {
        downstream_.receive(*storage_);
        storage_.reset();
      }
    }
  }

  void reset_target(auto& obj) {
    std::apply([&](auto&... c) { (c.reset(obj), ...); }, collectors_);
    downstream_.reset_target(obj);
  }
  [[nodiscard]] bool failed() const {
    return detail::any_failed(collectors_) || detail::failed(downstream_);
  }
};

template <typename Downstream, typename SinksTuple>
class FanoutSink {
  Downstream downstream_;
  SinksTuple sinks_;

 public:
  FanoutSink(Downstream downstream, SinksTuple sinks)
      : downstream_(std::move(downstream)), sinks_(std::move(sinks)) {}

  void on_start(std::string_view name, const char** attrs) {
    std::apply([&](auto&... s) { (s.on_start(name, attrs), ...); }, sinks_);
  }

  void on_text(std::string_view chunk) {
    std::apply([&](auto&... s) { (s.on_text(chunk), ...); }, sinks_);
  }

  void on_end(std::string_view name) {
    std::apply([&](auto&... s) { (s.on_end(name), ...); }, sinks_);
  }

  void receive(const auto& item) { downstream_.receive(item); }
  void done() { downstream_.done(); }
  void reset_target(auto& obj) {
    std::apply([&](auto&... s) { (s.reset_target(obj), ...); }, sinks_);
    downstream_.reset_target(obj);
  }
  [[nodiscard]] bool failed() const {
    return detail::any_failed(sinks_) || detail::failed(downstream_);
  }
};

// ----------------------------------------------------------------------------
// Factories
// ----------------------------------------------------------------------------

struct SelectFactory {
  std::string_view name;

  template <typename Downstream>
  auto operator()(Downstream downstream) const {
    return SelectSink<Downstream>(name, std::move(downstream));
  }
};

struct TextFactory {
  template <typename Downstream>
  auto operator()(Downstream downstream) const {
    return TextSink<Downstream>(std::move(downstream));
  }
};

// Forward declarations for bind expressions that produce collectors.
template <typename T, typename Member>
class AttrBind;

template <typename T, typename Member, typename PipelineFactory>
class FirstCollector;

template <typename T, typename Member, typename PipelineFactory>
class CollectCollector;

class AttrExpr {
  std::string_view name_;

 public:
  AttrExpr(std::string_view name) : name_(name) {}

  template <typename Downstream>
  auto operator()(Downstream downstream) const {
    return AttrSink<Downstream>(name_, std::move(downstream));
  }

  template <typename T, typename Member>
  auto make_collector(Member T::* member) const {
    return AttrBind<T, Member>(member, name_);
  }

  [[nodiscard]] std::string_view name() const { return name_; }
};

struct ParseXmlFactory {
  template <typename Downstream>
  auto operator()(Downstream downstream) const {
    return ParseXmlSink<Downstream>(std::move(downstream));
  }
};

template <typename Fn>
struct InvokeFactory {
  Fn fn;

  template <typename Downstream>
  auto operator()(Downstream /*downstream*/) const {
    return InvokeSink<Fn>(fn);
  }
};

template <typename T, typename... Binds>
struct ConstructFactory {
  std::tuple<Binds...> binds;

  template <typename Downstream>
  auto operator()(Downstream downstream) const {
    auto collectors = std::apply(
        [](const auto&... b) { return std::make_tuple(b.make_collector()...); },
        binds);
    return ConstructSink<T, Downstream, decltype(collectors)>(
        std::move(downstream), std::move(collectors));
  }
};

template <typename... Factories>
struct FanoutFactory {
  std::tuple<Factories...> factories;

  template <typename Downstream>
  auto operator()(Downstream downstream) const {
    auto sinks = std::apply(
        [&](const auto&... f) { return std::make_tuple(f(downstream)...); },
        factories);
    return FanoutSink<Downstream, decltype(sinks)>(std::move(downstream),
                                                   std::move(sinks));
  }
};

template <typename A, typename B>
struct PipeFactory {
  A a;
  B b;

  template <typename Downstream>
  auto operator()(Downstream downstream) const {
    return a(b(std::move(downstream)));
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

template <typename T, typename Member, typename PipelineFactory>
class FirstCollector {
  Member T::* member_;
  PipelineFactory factory_;
  using Pipeline = decltype(std::declval<const PipelineFactory&>()(
      std::declval<FirstWriter<T, Member>>()));
  Pipeline pipeline_;

 public:
  FirstCollector(Member T::* member, PipelineFactory factory)
      : member_(member),
        factory_(std::move(factory)),
        pipeline_(factory_(FirstWriter<T, Member>(member))) {}

  void bind_attrs(T&, const char**) {}
  void reset(T& obj) { pipeline_.reset_target(obj); }
  void on_start(std::string_view name, const char** attrs, T&) {
    pipeline_.on_start(name, attrs);
  }
  void on_text(std::string_view chunk, T&) { pipeline_.on_text(chunk); }
  void on_end(std::string_view name, T&) { pipeline_.on_end(name); }
  [[nodiscard]] bool failed() const { return detail::failed(pipeline_); }
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

template <typename T, typename Member, typename PipelineFactory>
class CollectCollector {
  Member T::* member_;
  PipelineFactory factory_;
  using Pipeline = decltype(std::declval<const PipelineFactory&>()(
      std::declval<CollectWriter<T, Member>>()));
  Pipeline pipeline_;

 public:
  CollectCollector(Member T::* member, PipelineFactory factory)
      : member_(member),
        factory_(std::move(factory)),
        pipeline_(factory_(CollectWriter<T, Member>(member))) {}

  void bind_attrs(T&, const char**) {}
  void reset(T& obj) { pipeline_.reset_target(obj); }
  void on_start(std::string_view name, const char** attrs, T&) {
    pipeline_.on_start(name, attrs);
  }
  void on_text(std::string_view chunk, T&) { pipeline_.on_text(chunk); }
  void on_end(std::string_view name, T&) { pipeline_.on_end(name); }
  [[nodiscard]] bool failed() const { return detail::failed(pipeline_); }
};

template <typename T, typename Member, typename Expr>
class BindSpec {
  Member T::* member_;
  Expr expr_;

 public:
  BindSpec(Member T::* member, Expr expr)
      : member_(member), expr_(std::move(expr)) {}

  auto make_collector() const {
    return expr_.template make_collector<T, Member>(member_);
  }
};

template <typename PipelineFactory>
class FirstExpr {
  PipelineFactory factory_;

 public:
  FirstExpr(PipelineFactory factory) : factory_(std::move(factory)) {}

  template <typename T, typename Member>
  auto make_collector(Member T::* member) const {
    return FirstCollector<T, Member, PipelineFactory>(member, factory_);
  }
};

template <typename PipelineFactory>
class CollectExpr {
  PipelineFactory factory_;

 public:
  CollectExpr(PipelineFactory factory) : factory_(std::move(factory)) {}

  template <typename T, typename Member>
  auto make_collector(Member T::* member) const {
    return CollectCollector<T, Member, PipelineFactory>(member, factory_);
  }
};

// ----------------------------------------------------------------------------
// User-facing helpers
// ----------------------------------------------------------------------------

inline auto Select(std::string_view name) { return SelectFactory{name}; }
inline auto Text() { return TextFactory{}; }
inline auto Attr(std::string_view name) { return AttrExpr{name}; }
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
  return FanoutFactory<Factories...>{std::move(factories)...};
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

template <typename Sink>
class XmlParser {
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
  explicit XmlParser(Sink sink) : sink_(std::move(sink)) {
    parser_ = XML_ParserCreate(nullptr);
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

  XmlParser(XmlParser&& other) noexcept
      : sink_(std::move(other.sink_)), parser_(other.parser_) {
    other.parser_ = nullptr;
    if (parser_) {
      XML_SetUserData(parser_, this);
    }
  }

  XmlParser& operator=(XmlParser&& other) noexcept {
    if (this != &other) {
      if (parser_) {
        XML_ParserFree(parser_);
      }
      sink_ = std::move(other.sink_);
      parser_ = other.parser_;
      other.parser_ = nullptr;
      if (parser_) {
        XML_SetUserData(parser_, this);
      }
    }
    return *this;
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
