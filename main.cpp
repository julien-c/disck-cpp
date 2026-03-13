#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    const char* home = std::getenv("HOME");
    if (!home) {
        std::cerr << "Cannot determine HOME directory\n";
        return 1;
    }

    fs::path hub = fs::path(home) / ".cache" / "huggingface" / "hub";

    if (!fs::is_directory(hub)) {
        std::cerr << "Not a directory: " << hub << "\n";
        return 1;
    }

    for (const auto& entry : fs::directory_iterator(hub)) {
        auto name = entry.path().filename().string();
        if (!entry.is_directory() || name[0] == '.')
            continue;

        std::cout << "\033[1m" << name << "\033[0m\n";

        // Print refs
        fs::path refs = entry.path() / "refs";
        if (fs::is_directory(refs)) {
            for (const auto& ref : fs::directory_iterator(refs)) {
                if (ref.is_regular_file())
                    std::cout << "  ref: " << ref.path().filename().string() << "\n";
            }
        }

        // Print snapshots
        fs::path snapshots = entry.path() / "snapshots";
        if (fs::is_directory(snapshots)) {
            for (const auto& snap : fs::directory_iterator(snapshots)) {
                if (!snap.is_directory())
                    continue;
                std::cout << "  snapshot: " << snap.path().filename().string() << "\n";
                for (const auto& file : fs::directory_iterator(snap.path())) {
                    auto rel = file.path().filename().string();
                    if (file.symlink_status().type() == fs::file_type::symlink) {
                        std::cout << "    " << rel << " -> " << fs::read_symlink(file.path()).string() << "\n";
                    } else if (file.is_regular_file()) {
                        std::cout << "    " << rel << "\n";
                    }
                }
            }
        }
    }
}

