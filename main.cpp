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
        if (entry.is_directory()) {
            std::cout << entry.path().filename().string() << "\n";
        }
    }
}
