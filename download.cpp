// download.cpp — Standalone C++ implementation of hf_hub_download.
// Dependency: cpp-httplib (vendored) + OpenSSL for HTTPS.
//
// Usage:
//   ./download <repo_id> <filename> [--token TOKEN] [--revision REV] [--repo-type TYPE] [--force]
//
// Examples:
//   ./download google/flan-t5-base config.json
//   ./download username/my-dataset data.csv --repo-type dataset

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "vendor/httplib.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

// --- Constants ---

static std::string get_env(const char * name, const std::string & fallback = "") {
    const char * v = std::getenv(name);
    return v ? v : fallback;
}

static std::string get_default_cache() {
    std::string v = get_env("HF_HUB_CACHE");
    if (!v.empty()) {
        return v;
    }
    v = get_env("HF_HOME");
    if (!v.empty()) {
        return v + "/hub";
    }
    std::string xdg = get_env("XDG_CACHE_HOME");
    if (xdg.empty()) {
        xdg = get_env("HOME") + "/.cache";
    }
    return xdg + "/huggingface/hub";
}

static const std::string ENDPOINT         = "https://huggingface.co";
static const std::string DEFAULT_REVISION = "main";
static const std::string DEFAULT_CACHE    = get_default_cache();
static const int         ETAG_TIMEOUT     = 10;
static const int         DOWNLOAD_TIMEOUT = 10;

// --- URL helpers ---

struct ParsedUrl {
    std::string scheme_host;  // e.g. "https://huggingface.co"
    std::string path;         // e.g. "/user/repo/resolve/main/file.json"
};

static ParsedUrl parse_url(const std::string & url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return { url, "/" };
    }
    auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        return { url, "/" };
    }
    return { url.substr(0, path_start), url.substr(path_start) };
}

static std::string url_encode(const std::string & s) {
    std::string result;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += (char) c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            result += buf;
        }
    }
    return result;
}

// Percent-encode each path component, preserving '/'
static std::string url_encode_path(const std::string & s) {
    std::string result;
    size_t      pos = 0;
    while (pos < s.size()) {
        auto slash = s.find('/', pos);
        if (slash == std::string::npos) {
            result += url_encode(s.substr(pos));
            break;
        }
        if (slash > pos) {
            result += url_encode(s.substr(pos, slash - pos));
        }
        result += '/';
        pos = slash + 1;
    }
    return result;
}

static std::string hf_hub_url(const std::string & repo_id,
                              const std::string & filename,
                              const std::string & repo_type,
                              const std::string & revision) {
    std::string rid = repo_id;
    if (repo_type == "dataset") {
        rid = "datasets/" + rid;
    } else if (repo_type == "space") {
        rid = "spaces/" + rid;
    }

    return ENDPOINT + "/" + rid + "/resolve/" + url_encode(revision.empty() ? DEFAULT_REVISION : revision) + "/" +
           url_encode_path(filename);
}

// --- Cache helpers ---

static std::string normalize_etag(const std::string & etag) {
    std::string e = etag;
    if (e.size() >= 2 && e[0] == 'W' && e[1] == '/') {
        e = e.substr(2);
    }
    if (e.size() >= 2 && e.front() == '"' && e.back() == '"') {
        e = e.substr(1, e.size() - 2);
    }
    return e;
}

static std::string repo_folder_name(const std::string & repo_id, const std::string & repo_type) {
    std::string result = repo_type + "s";
    size_t      pos    = 0;
    while (pos < repo_id.size()) {
        result += "--";
        auto slash = repo_id.find('/', pos);
        if (slash == std::string::npos) {
            result += repo_id.substr(pos);
            break;
        }
        result += repo_id.substr(pos, slash - pos);
        pos = slash + 1;
    }
    return result;
}

static bool is_commit_hash(const std::string & s) {
    if (s.size() != 40) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

static std::string extract_host(const std::string & url) {
    auto start = url.find("://");
    if (start == std::string::npos) {
        return "";
    }
    start += 3;
    auto end = url.find('/', start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

static std::string format_size(long long bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    }
    double       s       = bytes;
    const char * units[] = { "KB", "MB", "GB", "TB" };
    int          u       = -1;
    do {
        s /= 1024.0;
        u++;
    } while (s >= 1024.0 && u < 3);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %s", s, units[u]);
    return buf;
}

// --- Metadata via HEAD request ---

struct Metadata {
    std::string url_to_download;
    std::string etag;
    std::string commit_hash;
    long long   size;
    bool        strip_auth;
};

static Metadata get_metadata(const std::string & url, const std::string & token) {
    auto            parsed = parse_url(url);
    httplib::Client cli(parsed.scheme_host);
    cli.set_follow_location(false);
    cli.set_connection_timeout(ETAG_TIMEOUT);
    cli.set_read_timeout(ETAG_TIMEOUT);

    httplib::Headers hdrs = {
        { "User-Agent",      "hf_hub_download/standalone-cpp" },
        { "Accept-Encoding", "identity"                       },
    };
    if (!token.empty()) {
        hdrs.emplace("Authorization", "Bearer " + token);
    }

    auto res = cli.Head(parsed.path, hdrs);
    if (!res) {
        throw std::runtime_error("HEAD request failed: " + httplib::to_string(res.error()));
    }

    // Handle redirect: extract CDN URL from Location header
    std::string effective_url = url;
    if (res->status >= 300 && res->status < 400) {
        if (res->has_header("Location")) {
            effective_url = res->get_header_value("Location");
        }
    } else if (res->status >= 400) {
        throw std::runtime_error("HTTP " + std::to_string(res->status) + " for " + url);
    }

    Metadata meta;

    meta.commit_hash = res->get_header_value("X-Repo-Commit");
    if (meta.commit_hash.empty()) {
        throw std::runtime_error("Server did not return X-Repo-Commit header.");
    }

    std::string raw_etag = res->get_header_value("X-Linked-Etag");
    if (raw_etag.empty()) {
        raw_etag = res->get_header_value("ETag");
    }
    if (raw_etag.empty()) {
        throw std::runtime_error("Server did not return an ETag header.");
    }
    meta.etag = normalize_etag(raw_etag);

    std::string size_str = res->get_header_value("X-Linked-Size");
    if (size_str.empty()) {
        size_str = res->get_header_value("Content-Length");
    }
    if (size_str.empty()) {
        throw std::runtime_error("Server did not return Content-Length.");
    }
    meta.size = std::stoll(size_str);

    meta.url_to_download = effective_url;
    meta.strip_auth      = (url != effective_url && extract_host(url) != extract_host(effective_url));

    return meta;
}

// --- Streaming GET with resume and progress ---

static void http_get(const std::string & url,
                     const std::string & temp_path,
                     const std::string & token,
                     long long           expected_size,
                     const std::string & filename,
                     bool                force,
                     int                 retries = 5) {
    long long resume_size = 0;
    if (!force && fs::exists(temp_path)) {
        resume_size = (long long) fs::file_size(temp_path);
        if (resume_size == expected_size) {
            return;
        }
    }

    FILE * fp = fopen(temp_path.c_str(), resume_size > 0 ? "r+b" : "wb");
    if (!fp) {
        throw std::runtime_error("Cannot open " + temp_path);
    }
    if (resume_size > 0) {
        fseek(fp, 0, SEEK_END);
    }

    auto            parsed = parse_url(url);
    httplib::Client cli(parsed.scheme_host);
    cli.set_follow_location(true);
    cli.set_connection_timeout(DOWNLOAD_TIMEOUT);
    cli.set_read_timeout(DOWNLOAD_TIMEOUT);

    httplib::Headers hdrs = {
        { "User-Agent", "hf_hub_download/standalone-cpp" },
    };
    if (!token.empty()) {
        hdrs.emplace("Authorization", "Bearer " + token);
    }
    if (resume_size > 0) {
        hdrs.emplace("Range", "bytes=" + std::to_string(resume_size) + "-");
    }

    // Truncated filename for display
    std::string display_name = filename;
    if (display_name.size() > 40) {
        display_name = "(...)" + display_name.substr(display_name.size() - 35);
    }

    long long current_resume = resume_size;

    auto res = cli.Get(
        parsed.path, hdrs,
        // Response handler: check if server returned 200 instead of 206
        [&](const httplib::Response & response) -> bool {
            if (current_resume > 0 && response.status == 200) {
                fseek(fp, 0, SEEK_SET);
                ftruncate(fileno(fp), 0);
                current_resume = 0;
            }
            return true;
        },
        // Content receiver
        [&](const char * data, size_t len) -> bool { return fwrite(data, 1, len, fp) == len; },
        // Progress
        [&](uint64_t current, uint64_t /*total*/) -> bool {
            long long now    = (long long) current + current_resume;
            double    frac   = std::min((double) now / expected_size, 1.0);
            int       width  = 40;
            int       filled = (int) (frac * width);

            char bar[41];
            memset(bar, '#', filled);
            memset(bar + filled, '-', width - filled);
            bar[width] = '\0';

            fprintf(stderr, "\r%-40s [%s] %s / %s  ", display_name.c_str(), bar, format_size(now).c_str(),
                    format_size(expected_size).c_str());
            fflush(stderr);
            return true;
        });

    fclose(fp);
    fprintf(stderr, "\n");

    if (!res) {
        auto err = res.error();
        if (retries > 0 && (err == httplib::Error::Connection || err == httplib::Error::Read ||
                            err == httplib::Error::ConnectionTimeout)) {
            fprintf(stderr, "Retrying (%d left)...\n", retries - 1);
            http_get(url, temp_path, token, expected_size, filename, false, retries - 1);
            return;
        }
        throw std::runtime_error("Download failed: " + httplib::to_string(err));
    }
    if (res->status >= 400) {
        throw std::runtime_error("HTTP " + std::to_string(res->status) + " downloading " + url);
    }

    long long final_size = (long long) fs::file_size(temp_path);
    if (expected_size > 0 && final_size != expected_size) {
        throw std::runtime_error("Size mismatch: expected " + std::to_string(expected_size) + ", got " +
                                 std::to_string(final_size));
    }
}

// --- Symlink ---

static void create_symlink(const std::string & src, const std::string & dst) {
    std::error_code ec;
    fs::remove(dst, ec);
    fs::path rel = fs::relative(fs::path(src), fs::path(dst).parent_path(), ec);
    if (!ec) {
        fs::create_symlink(rel, dst, ec);
    }
    if (ec) {
        // Fallback: copy (e.g. on filesystems without symlink support)
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    }
}

// --- Main download function ---

static std::string hf_hub_download(const std::string & repo_id,
                                   const std::string & filename,
                                   const std::string & repo_type,
                                   const std::string & revision,
                                   const std::string & token,
                                   bool                force_download) {
    std::string rev     = revision.empty() ? DEFAULT_REVISION : revision;
    std::string rtype   = repo_type.empty() ? "model" : repo_type;
    std::string storage = DEFAULT_CACHE + "/" + repo_folder_name(repo_id, rtype);

    // Fast path: exact commit hash already cached
    if (is_commit_hash(rev)) {
        fs::path pp = fs::path(storage) / "snapshots" / rev / filename;
        if (fs::exists(pp) && !force_download) {
            return pp.string();
        }
    }

    // Get metadata from Hub
    std::string url  = hf_hub_url(repo_id, filename, rtype, rev);
    auto        meta = get_metadata(url, token);

    std::string blob_path = storage + "/blobs/" + meta.etag;
    fs::path    pp        = fs::path(storage) / "snapshots" / meta.commit_hash / filename;

    fs::create_directories(fs::path(blob_path).parent_path());
    fs::create_directories(pp.parent_path());

    // Store ref: revision -> commit_hash
    if (!is_commit_hash(rev)) {
        std::string ref_path = storage + "/refs/" + rev;
        fs::create_directories(fs::path(ref_path).parent_path());
        std::ofstream(ref_path) << meta.commit_hash;
    }

    // Already cached?
    if (!force_download && fs::exists(pp)) {
        return pp.string();
    }
    if (!force_download && fs::exists(blob_path)) {
        create_symlink(blob_path, pp.string());
        return pp.string();
    }

    // Download to .incomplete, then move to blob
    std::string incomplete = blob_path + ".incomplete";
    if (force_download) {
        std::error_code ec;
        fs::remove(incomplete, ec);
    }

    std::string dl_token = meta.strip_auth ? "" : token;
    http_get(meta.url_to_download, incomplete, dl_token, meta.size, filename, force_download);

    fs::rename(incomplete, blob_path);
    create_symlink(blob_path, pp.string());

    return pp.string();
}

// --- CLI ---

static void usage() {
    fprintf(stderr,
            "Usage: download <repo_id> <filename> [options]\n"
            "\n"
            "Options:\n"
            "  --token TOKEN      HF API token (or set HF_TOKEN env var)\n"
            "  --revision REV     Branch, tag, or commit hash (default: main)\n"
            "  --repo-type TYPE   model, dataset, or space (default: model)\n"
            "  --force            Re-download even if cached\n");
}

int main(int argc, char * argv[]) {
    if (argc < 3) {
        usage();
        return 1;
    }

    std::string repo_id, filename, token, revision, repo_type;
    bool        force      = false;
    int         positional = 0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--token" && i + 1 < argc) {
            token = argv[++i];
        } else if (arg == "--revision" && i + 1 < argc) {
            revision = argv[++i];
        } else if (arg == "--repo-type" && i + 1 < argc) {
            repo_type = argv[++i];
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else if (arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            usage();
            return 1;
        } else if (positional == 0) {
            repo_id = arg;
            positional++;
        } else if (positional == 1) {
            filename = arg;
            positional++;
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", arg.c_str());
            usage();
            return 1;
        }
    }

    if (repo_id.empty() || filename.empty()) {
        usage();
        return 1;
    }

    if (token.empty()) {
        token = get_env("HF_TOKEN");
    }

    try {
        std::string path = hf_hub_download(repo_id, filename, repo_type, revision, token, force);
        std::cout << path << "\n";
    } catch (const std::exception & e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    return 0;
}
