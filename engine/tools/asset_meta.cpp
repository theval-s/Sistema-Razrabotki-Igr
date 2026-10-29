#include <engine/assets/AssetCatalog.hpp>

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3 || (std::string(argv[1]) != "generate" && std::string(argv[1]) != "check")) {
        std::cerr << "Usage: sri-asset-meta generate <asset-root> | check <asset-root>\n";
        return 2;
    }
    const std::string bytes = argv[2];
    const std::filesystem::path root = std::u8string(bytes.begin(), bytes.end());
    std::string error;
    std::size_t count = 0;
    if (std::string(argv[1]) == "generate" && !engine::GenerateMissingAssetMetadata(root, count, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    engine::AssetCatalog catalog;
    if (!catalog.Scan(root, {}, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "Created " << count << " sidecars; catalog contains " << catalog.Size() << " assets\n";
}
