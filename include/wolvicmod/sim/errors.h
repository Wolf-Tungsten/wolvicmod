#pragma once

#include <stdexcept>
#include <string>

namespace wolvicmod {

// Unified diagnostic type (§6.2): every modeling error throws wolvicmod::Error
// with hierarchical paths in the message.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace detail {
[[noreturn]] inline void fail(const std::string& msg) { throw Error(msg); }
}  // namespace detail

}  // namespace wolvicmod
