// Copyright 2022 Huawei Cloud Computing Technology Co., Ltd.
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

#include "src/buildtool/main/install_cas.hpp"

#include <optional>

#include "catch2/catch_test_macros.hpp"
#include "src/buildtool/common/artifact.hpp"
#include "src/buildtool/crypto/hash_function.hpp"

TEST_CASE("ObjectInfoFromLiberalString", "[artifact]") {
    auto expected = *Artifact::ObjectInfo::FromString(
        HashFunction::Type::GitSHA1,
        "[5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689:11:f]");
    auto expected_as_tree = *Artifact::ObjectInfo::FromString(
        HashFunction::Type::GitSHA1,
        "[5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689:0:t]");

    // Check (default) file hashes
    CHECK(ObjectInfoFromLiberalString(
              HashFunction::Type::GitSHA1,
              "[5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689:11:f]",
              /*has_remote=*/false) == expected);
    CHECK(ObjectInfoFromLiberalString(
              HashFunction::Type::GitSHA1,
              "5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689:11:f]",
              /*has_remote=*/false) == expected);
    CHECK(ObjectInfoFromLiberalString(
              HashFunction::Type::GitSHA1,
              "[5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689:11:f",
              /*has_remote=*/false) == expected);
    CHECK(ObjectInfoFromLiberalString(
              HashFunction::Type::GitSHA1,
              "5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689:11:f",
              /*has_remote=*/false) == expected);
    CHECK(ObjectInfoFromLiberalString(
              HashFunction::Type::GitSHA1,
              "5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689:11:file",
              /*has_remote=*/false) == expected);
    CHECK(ObjectInfoFromLiberalString(
              HashFunction::Type::GitSHA1,
              "5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689:11:notavalidletter",
              /*has_remote=*/false) == expected);

    // Without size, which is not honored in equality
    CHECK(
        ObjectInfoFromLiberalString(HashFunction::Type::GitSHA1,
                                    "5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689",
                                    /*has_remote=*/false) == expected);
    CHECK(
        ObjectInfoFromLiberalString(HashFunction::Type::GitSHA1,
                                    "5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689:",
                                    /*has_remote=*/false) == expected);

    // Syntactically invalid size should be ignored
    CHECK(ObjectInfoFromLiberalString(
              HashFunction::Type::GitSHA1,
              "5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689:xyz",
              /*has_remote=*/false) == expected);

    // Check tree hashes
    CHECK(ObjectInfoFromLiberalString(
              HashFunction::Type::GitSHA1,
              "5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689::t",
              /*has_remote=*/false) == expected_as_tree);
    CHECK(ObjectInfoFromLiberalString(
              HashFunction::Type::GitSHA1,
              "5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689::tree",
              /*has_remote=*/false) == expected_as_tree);
    CHECK(ObjectInfoFromLiberalString(
              HashFunction::Type::GitSHA1,
              "5e1c309dae7f45e0f39b1bf3ac3cd9db12e7d689:xyz:t",
              /*has_remote=*/false) == expected_as_tree);
}
