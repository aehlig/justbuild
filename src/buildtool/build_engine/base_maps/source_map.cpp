#include "src/buildtool/build_engine/base_maps/source_map.hpp"

#include <filesystem>

#include "src/buildtool/common/artifact_digest.hpp"
#include "src/buildtool/file_system/object_type.hpp"
#include "src/buildtool/multithreading/async_map_consumer.hpp"
#include "src/utils/cpp/json.hpp"

namespace BuildMaps::Base {

namespace {

auto as_target(const BuildMaps::Base::EntityName& key, ExpressionPtr artifact)
    -> AnalysedTargetPtr {
    auto stage =
        ExpressionPtr{Expression::map_t{key.name, std::move(artifact)}};
    return std::make_shared<AnalysedTarget>(
        TargetResult{stage, Expression::kEmptyMap, stage},
        std::vector<ActionDescription::Ptr>{},
        std::vector<std::string>{},
        std::vector<Tree::Ptr>{},
        std::unordered_set<std::string>{},
        std::set<std::string>{});
}

}  // namespace

auto CreateSourceTargetMap(const gsl::not_null<DirectoryEntriesMap*>& dirs,
                           std::size_t jobs) -> SourceTargetMap {
    auto src_target_reader = [dirs](auto ts,
                                    auto setter,
                                    auto logger,
                                    auto /* unused */,
                                    auto const& key) {
        using std::filesystem::path;
        auto name = path(key.name).lexically_normal();
        if (name.is_absolute() or *name.begin() == "..") {
            (*logger)(
                fmt::format("Source file reference outside current module: {}",
                            key.name),
                true);
            return;
        }
        auto dir = (path(key.module) / name).parent_path();
        auto const* ws_root =
            RepositoryConfig::Instance().WorkspaceRoot(key.repository);

        auto src_file_reader = [ts, key, name, setter, logger, dir, ws_root](
                                   bool exists_in_ws_root) {
            if (ws_root != nullptr and exists_in_ws_root) {
                if (auto desc = ws_root->ToArtifactDescription(
                        path(key.module) / name, key.repository)) {
                    (*setter)(as_target(key, ExpressionPtr{std::move(*desc)}));
                    return;
                }
            }
            (*logger)(fmt::format("Cannot determine source file {}",
                                  path(key.name).filename().string()),
                      true);
        };

        if (ws_root != nullptr and ws_root->HasFastDirectoryLookup()) {
            // by-pass directory map and directly attempt to read from ws_root
            src_file_reader(ws_root->IsFile(path(key.module) / name));
            return;
        }
        dirs->ConsumeAfterKeysReady(
            ts,
            {ModuleName{key.repository, dir.string()}},
            [key, src_file_reader](auto values) {
                src_file_reader(
                    values[0]->Contains(path(key.name).filename().string()));
            },
            [logger, dir](auto msg, auto fatal) {
                (*logger)(
                    fmt::format(
                        "While reading contents of {}: {}", dir.string(), msg),
                    fatal);
            }

        );
    };
    return AsyncMapConsumer<EntityName, AnalysedTargetPtr>(src_target_reader,
                                                           jobs);
}

};  // namespace BuildMaps::Base
