/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// FloxError — structured exception for FLOX core code.
//
// Each FloxError carries a stable code (e.g. "E_SYM_001"), a human message,
// and a help URL pointing to the corresponding documentation page in the
// FLOX site. Bindings translate this into their language-native exception
// type (FloxError(Exception) in Python, FloxError extends Error in Node,
// etc.) while preserving the three fields.
//
// Codes follow the convention E_<DOMAIN>_<NUMBER> (3 digits). DOMAIN is a
// short uppercase tag identifying the subsystem: SYM (registry), DATA
// (input streams), KEY (event field lookup), TIME (calendar/intervals),
// LEN (array length mismatches), ORDER, RISK, IDX, IO, CONFIG, ...
//
// Migration policy is **incremental**: new throw sites use FloxError;
// existing std::invalid_argument / std::runtime_error sites stay until
// touched. Each code committed to public surface gets a Markdown page in
// docs/errors/<code>.md (CI gate, scripts/check_error_codes.py).

#include <stdexcept>
#include <string>
#include <utility>

namespace flox
{

class FloxError : public std::runtime_error
{
 public:
  FloxError(std::string code, std::string message, std::string helpUrl)
      : std::runtime_error(message),
        _code(std::move(code)),
        _message(std::move(message)),
        _helpUrl(std::move(helpUrl))
  {
  }

  FloxError(std::string code, std::string message)
      : std::runtime_error(message),
        _code(std::move(code)),
        _message(std::move(message)),
        _helpUrl(defaultHelpUrl(_code))
  {
  }

  const std::string& code() const noexcept { return _code; }
  const std::string& message() const noexcept { return _message; }
  const std::string& helpUrl() const noexcept { return _helpUrl; }

  // Build the canonical help-URL for a given code. Bindings reuse this so
  // the URL convention is one definition rather than scattered string
  // builders.
  static std::string defaultHelpUrl(const std::string& code)
  {
    return "https://flox-foundation.github.io/flox/errors/" + code + "/";
  }

 private:
  std::string _code;
  std::string _message;
  std::string _helpUrl;
};

}  // namespace flox
