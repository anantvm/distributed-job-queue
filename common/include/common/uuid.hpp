#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// uuid.hpp
//
// Generates RFC 4122 version-4 (random) UUID strings.
// Thread-safe: uses a thread_local RNG per thread.
//
// Example output: "550e8400-e29b-41d4-a716-446655440000"
// ─────────────────────────────────────────────────────────────────────────────

#include <string>

namespace uuid {

// Returns a new random UUID v4 string (format: 8-4-4-4-12 hex chars).
[[nodiscard]] std::string generate();

} // namespace uuid
