// Copyright 2023 Huawei Cloud Computing Technology Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef INCLUDED_SRC_OTHER_TOOLS_UTILS_CURL_URL_HANDLE_HPP
#define INCLUDED_SRC_OTHER_TOOLS_UTILS_CURL_URL_HANDLE_HPP

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gsl/gsl"
#include "src/other_tools/utils/curl_context.hpp"

extern "C" {
using CURLU = struct Curl_URL;
}

class CurlURLHandle;
using CurlURLHandlePtr = std::shared_ptr<CurlURLHandle>;

/// \brief Type describing a possibly missing string. Used to store a retrieved
/// field of a parsed URL.
using OptionalString = std::optional<std::string>;

void curl_url_closer(gsl::owner<CURLU*> handle);

struct GitConfigKey {
    OptionalString scheme{std::nullopt};
    OptionalString user{std::nullopt};
    // might contain wildcards
    OptionalString host{std::nullopt};
    OptionalString port{std::nullopt};
    // will include query and fragment, if existing
    std::filesystem::path path{"/"};
};

using GitConfigKeyPtr = std::shared_ptr<GitConfigKey>;

/// \brief Structure storing the information needed to quantify precedence with
/// respect to the gitconfig keys matching rules.
/// Non-default values are set ONLY if matching rules are satisfied.
struct ConfigKeyMatchDegree {
    // if a matching happened;
    bool matched{false};
    // length of config key's host field if host was matched
    std::size_t host_len{};
    // length of config key's path field if path was matched;
    // comparison ends on a '/' char or the end of the path
    std::size_t path_len{};
    // signals a match for the user field between config key and remote URL,
    // only if user field exists in config key
    bool user_matched{false};
};

/// \brief Stores the components of a valid no_proxy envariable pattern
struct NoproxyPattern {
    // stores the substrings of the host portion of the pattern, obtained by
    // splitting with delimiter '.'
    std::vector<std::string> host_tokens;
    // port number as string, or nullopt if port missing
    std::optional<std::string> port;
};

/// \brief Class handling URLs using libcurl API.
/// As with libcurl, only limited checks are performed in order to parse the
/// required fields for a given URL string.
class CurlURLHandle {
  public:
    CurlURLHandle() noexcept = default;
    ~CurlURLHandle() noexcept = default;

    // prohibit moves & copies
    CurlURLHandle(CurlURLHandle const&) = delete;
    CurlURLHandle(CurlURLHandle&& other) = delete;
    auto operator=(CurlURLHandle const&) = delete;
    auto operator=(CurlURLHandle&& other) = delete;

    /// \brief Creates a CurlURLHandle object by parsing the given URL.
    /// It performs also a normalization step of the path. Requires the protocol
    /// to be explicitly specified, i.e., it must have a non-empty scheme field.
    /// \returns Pointer to created object, nullptr on failure to parse, or
    /// nullopt on an unexpected exception.
    [[nodiscard]] auto static Create(std::string const& url) noexcept
        -> std::optional<CurlURLHandlePtr>;

    /// \brief Creates a CurlURLHandle object by parsing the given URL.
    /// It allows the user to be very permissive with the types of URL strings
    /// it can parse by providing configuration arguments that mirror those
    /// provided by the libcurl API (see libcurl docs for effects of each flag).
    /// \param [in] ignore_fatal Do not log failure if error or exception.
    /// \returns Pointer to created object, nullptr on failure to parse with
    /// given arguments, or nullopt on an unexpected exception.
    [[nodiscard]] auto static CreatePermissive(
        std::string const& url,
        bool use_guess_scheme = false,
        bool use_default_scheme = false,
        bool use_non_support_scheme = false,
        bool use_no_authority = false,
        bool use_path_as_is = false,
        bool use_allow_space = false,
        bool ignore_fatal = false) noexcept -> std::optional<CurlURLHandlePtr>;

    /// \brief Creates a duplicate CurlURLHandle object.
    /// \returns Pointer to duplicated object, or nullptr on errors.
    [[nodiscard]] auto Duplicate() noexcept -> CurlURLHandlePtr;

    /// \brief Recomposes the URL from the fields in the stored handle.
    /// Flags parallel the libcurl API for handling the scheme and port fields.
    /// \param [in] ignore_fatal Do not log failure if error or exception.
    /// \returns The recomposed URL as a string, or nullopt on errors.
    [[nodiscard]] auto GetURL(bool use_default_port = false,
                              bool use_default_scheme = false,
                              bool use_no_default_port = false,
                              bool ignore_fatal = false) noexcept
        -> std::optional<std::string>;

    /// \brief Gets the parsed scheme field.
    /// \returns Nullopt on errors, or an OptionalString containing either the
    /// existing stored scheme or nullopt if scheme field is missing.
    [[nodiscard]] auto GetScheme(bool use_default_scheme = false) noexcept
        -> std::optional<OptionalString>;

    /// \brief While libcurl's URL API correctly checks that valid hostnames
    /// don't contain special characters, gitconfig key URLs (*.<key>.*) allow
    /// asterisks ('*'). This function recognizes such hostnames and returns a
    /// struct containing all the relevant parsed fields required for matching.
    /// \returns Pointer to said struct, nullopt if errors, nullptr if
    /// unparsable.
    [[nodiscard]] auto static ParseConfigKey(std::string const& key) noexcept
        -> std::optional<GitConfigKeyPtr>;

    /// \brief Parses a given gitconfig key url component (e.g., http.<key>.*)
    /// and returns to what degree it matches the stored URL.
    /// In particular, a non-parsable key returns a non-match.
    /// \returns Matching degree struct, or nullopt on errors.
    [[nodiscard]] auto MatchConfigKey(std::string const& key) noexcept
        -> std::optional<ConfigKeyMatchDegree>;

    /// \brief Checks if the stored URL matches a given "no_proxy"-style string.
    /// \returns Whether a match was found, or nullopt on errors.
    [[nodiscard]] auto NoproxyStringMatches(
        std::string const& no_proxy) noexcept -> std::optional<bool>;

    /// \brief Gets the hostname from URL.
    /// \returns The host name or std::nullopt if missing or on errors.
    [[nodiscard]] static auto GetHostname(std::string const& url) noexcept
        -> std::optional<std::string>;

  private:
    // IMPORTANT: the CurlContext must be initialized before any curl
    // object!
    CurlContext curl_context_;
    std::unique_ptr<CURLU, decltype(&curl_url_closer)> handle_{nullptr,
                                                               curl_url_closer};

    /// \brief Try to parse the given key as a valid URL and, if successful,
    /// populate a struct with the parsed components needed for config matching.
    [[nodiscard]] auto static GetConfigStructFromKey(
        std::string const& key) noexcept -> std::optional<GitConfigKeyPtr>;
};

#endif  // INCLUDED_SRC_OTHER_TOOLS_UTILS_CURL_URL_HANDLE_HPP
