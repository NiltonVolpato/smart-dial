// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <concepts>

namespace coprocessor {

class BaseHandler;

/// An object and one of its `void()` methods: what the reactor runs. No heap,
/// no captures: state lives in the object.
///
/// Every callback has an owner, the handler it works for, so cancelling a
/// call can find (and delete) whoever is waiting on its behalf. Handlers own
/// their own callbacks; components inside a handler, like HttpClient, pass
/// theirs explicitly. A null owner is never cancelled.
class Callback {
 public:
  Callback() = default;

  /// A handler's own method, e.g. `Callback(this, &ActionHandler::done)`.
  template <class T, class U>
    requires std::derived_from<T, U> && std::derived_from<T, BaseHandler>
  Callback(T* handler, void (U::*method)())
      : Callback(handler, method, handler) {}

  /// A method of some object working for `owner`.
  template <class T, class U>
    requires std::derived_from<T, U>
  Callback(T* object, void (U::*method)(), BaseHandler* owner)
      : object_(static_cast<U*>(object)),
        // Converting a member pointer and back is lossless: invoke<U>
        // restores it.
        method_(reinterpret_cast<AnyMethod>(method)),
        invoke_(&invoke<U>),
        owner_(owner) {}

  void operator()() const { invoke_(object_, method_); }

  explicit operator bool() const { return invoke_ != nullptr; }

  [[nodiscard]] BaseHandler* owner() const { return owner_; }

 private:
  class Any;
  using AnyMethod = void (Any::*)();

  template <class U>
  static void invoke(void* object, AnyMethod method) {
    (static_cast<U*>(object)->*reinterpret_cast<void (U::*)()>(method))();
  }

  void* object_ = nullptr;
  AnyMethod method_ = nullptr;
  void (*invoke_)(void*, AnyMethod) = nullptr;
  BaseHandler* owner_ = nullptr;
};

}  // namespace coprocessor
