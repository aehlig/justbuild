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

#include "src/buildtool/execution_api/remote/bazel/bytestream_client.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <grpc/grpc.h>

#include "catch2/catch_test_macros.hpp"
#include "gsl/gsl"
#include "src/buildtool/common/artifact_digest.hpp"
#include "src/buildtool/common/artifact_digest_factory.hpp"
#include "src/buildtool/common/remote/remote_common.hpp"
#include "src/buildtool/crypto/hash_function.hpp"
#include "src/buildtool/execution_api/common/artifact_blob.hpp"
#include "src/buildtool/execution_api/remote/config.hpp"
#include "src/buildtool/file_system/object_type.hpp"
#include "test/utils/hermeticity/test_hash_function_type.hpp"
#include "test/utils/remote_execution/test_auth_config.hpp"
#include "test/utils/remote_execution/test_remote_config.hpp"

TEST_CASE("ByteStream Client: Transfer single blob", "[execution_api]") {
    auto auth_config = TestAuthConfig::ReadFromEnvironment();
    REQUIRE(auth_config);

    auto remote_config = TestRemoteConfig::ReadFromEnvironment();
    REQUIRE(remote_config);
    REQUIRE(remote_config->remote_address);

    auto stream = ByteStreamClient{remote_config->remote_address->host,
                                   remote_config->remote_address->port,
                                   &*auth_config};

    HashFunction const hash_function{TestHashType::ReadFromEnvironment()};

    SECTION("Upload small blob") {
        std::string instance_name{"remote-execution"};
        std::string content("foobar");

        // digest of "foobar"
        auto digest = ArtifactDigestFactory::HashDataAs<ObjectType::File>(
            hash_function, content);

        ArtifactBlob const blob{digest, content, /*is_exec=*/false};
        CHECK(stream.Write(instance_name, blob));

        auto const downloaded_blob = stream.Read(instance_name, digest);
        REQUIRE(downloaded_blob.has_value());
        CHECK(*downloaded_blob->data == content);
    }

    SECTION("Small blob with wrong digest") {
        std::string instance_name{"remote-execution"};
        std::string content("foobar");
        std::string other_content("This is a differnt string");

        // Valid digest, but for a different string
        auto digest = ArtifactDigestFactory::HashDataAs<ObjectType::File>(
            hash_function, other_content);
        ArtifactBlob const blob{digest, content, /*is_exec=*/false};

        CHECK(not stream.Write(instance_name, blob));
    }

    SECTION("Upload large blob") {
        static constexpr std::size_t kLargeSize =
            GRPC_DEFAULT_MAX_RECV_MESSAGE_LENGTH + 1;
        std::string instance_name{"remote-execution"};

        std::string content(kLargeSize, '\0');
        for (std::size_t i{}; i < content.size(); ++i) {
            content[i] = instance_name[i % instance_name.size()];
        }

        // digest of "instance_nameinstance_nameinstance_..."
        auto digest = ArtifactDigestFactory::HashDataAs<ObjectType::File>(
            hash_function, content);
        ArtifactBlob const blob{digest, content, /*is_exec=*/false};

        CHECK(stream.Write(instance_name, blob));

        SECTION("Download large blob") {
            auto const downloaded_blob = stream.Read(instance_name, digest);
            REQUIRE(downloaded_blob.has_value());
            CHECK(*downloaded_blob->data == content);
        }

        SECTION("Incrementally download large blob") {
            auto reader = stream.IncrementalRead(instance_name, digest);

            std::string data{};
            auto chunk = reader.Next();
            while (chunk and not chunk->empty()) {
                data.append(chunk->begin(), chunk->end());
                chunk = reader.Next();
            }

            CHECK(chunk);
            CHECK(data == content);
        }
    }
}
