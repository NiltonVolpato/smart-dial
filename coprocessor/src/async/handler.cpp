// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include "async/handler.h"

#include "async/reactor.h"

namespace coprocessor {

BaseHandler::~BaseHandler() { reactor_.forget(this); }

}  // namespace coprocessor
