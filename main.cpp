#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Split a string by a delimiter
static std::vector<std::string> split(const std::string & s, const std::string & delim) {
    std::vector<std::string> parts;
    size_t                   pos = 0;
    while (pos < s.size()) {
        auto found = s.find(delim, pos);
        if (found == std::string::npos) {
            parts.push_back(s.substr(pos));
            break;
        }
        parts.push_back(s.substr(pos, found - pos));
        pos = found + delim.size();
    }
    return parts;
}

// Build an HF URL from a cache folder name like "models--julien-c--EsperBERTo-small-pos"
static std::string hf_url(const std::string & name) {
    auto parts = split(name, "--");
    if (parts.size() < 2) {
        return "";
    }
    // parts[0] is the type: models, datasets, spaces
    // remaining parts joined by "/" form the repo id
    std::string url = "https://huggingface.co/";
    if (parts[0] != "models") {
        url += parts[0] + "/";
    }
    for (size_t i = 1; i < parts.size(); i++) {
        if (i > 1) {
            url += "/";
        }
        url += parts[i];
    }
    return url;
}

// Terminal hyperlink: OSC 8 ; params ; uri ST text OSC 8 ;; ST
static std::string hyperlink(const std::string & url, const std::string & text) {
    return "\033]8;;" + url + "\033\\" + text + "\033]8;;\033\\";
}

int main(int argc, char * argv[]) {
    const char * home = std::getenv("HOME");
    if (!home) {
        std::cerr << "Cannot determine HOME directory\n";
        return 1;
    }

    fs::path hub = fs::path(home) / ".cache" / "huggingface" / "hub";

    if (argc > 1 && std::string(argv[1]) == "--link") {
        fs::create_symlink(hub, "hub");
        std::cout << "Created symlink: hub -> " << hub.string() << "\n";
        return 0;
    }

    if (!fs::is_directory(hub)) {
        std::cerr << "Not a directory: " << hub << "\n";
        return 1;
    }

    for (const auto & entry : fs::directory_iterator(hub)) {
        auto name = entry.path().filename().string();
        if (!entry.is_directory() || name[0] == '.') {
            continue;
        }

        auto url = hf_url(name);
        if (!url.empty()) {
            std::cout << "\033[1;94m" << hyperlink(url, name) << "\033[0m\n";
        } else {
            std::cout << "\033[1;94m" << name << "\033[0m\n";
        }

        // Print refs
        fs::path refs = entry.path() / "refs";
        if (fs::is_directory(refs)) {
            for (const auto & ref : fs::directory_iterator(refs)) {
                if (ref.is_regular_file()) {
                    std::cout << "  ref: " << ref.path().filename().string() << "\n";
                }
            }
        }

        // Print snapshots
        fs::path snapshots = entry.path() / "snapshots";
        if (fs::is_directory(snapshots)) {
            for (const auto & snap : fs::directory_iterator(snapshots)) {
                if (!snap.is_directory()) {
                    continue;
                }
                std::cout << "  snapshot: " << snap.path().filename().string() << "\n";
                for (const auto & file : fs::directory_iterator(snap.path())) {
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
