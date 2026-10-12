// ngen-packer-usd: packs one USD file (.usda, .usdc, .usd) into a layer pack, the opinions the file authors, and writes each mesh and
// material the file defines as a secondary output, "<asset>#<prim path>" (src/asset/README.md).

#include "packer.h"
#include "scenepack.h"
#include "usdpacker.h"

#include <format>
#include <fstream>
#include <print>

auto main(int argc, char** argv) -> int {
    auto args = parsePackerArgs(argc, argv);
    if (!args) {
        std::println(stderr, "ngen-packer-usd: {}", args.error());
        return 2;
    }

    auto result = packUsdFile(args->asset, args->source);
    if (!result.error.empty()) {
        std::println(stderr, "{}", result.error);
        return 1;
    }

    clearSecondaryOutputs(args->out);
    std::vector<std::pair<std::string, std::string>> outputs;
    auto writeSecondary = [&](const std::string& primPath, const std::vector<std::byte>& bytes) -> bool {
        auto file = secondaryOutputFile(args->out, primPath);
        auto written = writePackerFile(file, bytes);
        if (!written) {
            std::println(stderr, "{}", written.error());
            return false;
        }
        outputs.emplace_back(args->asset + "#" + primPath, file);
        return true;
    };
    for (const auto& [primPath, mesh] : result.meshes) {
        if (!writeSecondary(primPath, writeMeshPack(mesh))) {
            return 1;
        }
    }
    for (const auto& [primPath, material] : result.materials) {
        if (!writeSecondary(primPath, writeMaterialPack(material))) {
            return 1;
        }
    }
    if (auto listed = writeSecondaryOutputs(args->out, outputs); !listed) {
        std::println(stderr, "{}", listed.error());
        return 1;
    }
    if (auto written = writePackerFile(args->out, writeLayerPack(result.layer)); !written) {
        std::println(stderr, "{}", written.error());
        return 1;
    }

    // The file is the only input: nothing from another file is read.
    std::ofstream depfile(args->depfile, std::ios::trunc);
    depfile << args->out << ": " << args->source << '\n';
    if (!depfile) {
        std::println(stderr, "cannot write {}", args->depfile);
        return 1;
    }

    // Warnings on stdout: the asset server shows them in its trace; the pack keeps them too.
    for (const auto& warning : result.layer.warnings) {
        if (warning.path.empty()) {
            std::println("{}", warning.text);
        } else {
            std::println("{}: {}", warning.path, warning.text);
        }
    }
    return 0;
}
