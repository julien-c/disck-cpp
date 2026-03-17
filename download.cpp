// download.cpp — Standalone C++ implementation of hf_hub_download.
// Minimal dependency: libcurl (pre-installed on macOS and most Linux).
//
// Usage:
//   ./download <repo_id> <filename> [--token TOKEN] [--revision REV] [--repo-type TYPE] [--force]
//
// Examples:
//   ./download google/flan-t5-base config.json
//   ./download username/my-dataset data.csv --repo-type dataset

#include <curl/curl.h>
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

static std::string get_endpoint() {
    std::string s = get_env("HF_ENDPOINT", "https://huggingface.co");
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
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

static const std::string ENDPOINT         = get_endpoint();
static const std::string DEFAULT_REVISION = "main";
static const std::string DEFAULT_CACHE    = get_default_cache();
static const long        ETAG_TIMEOUT     = 10;
static const long        DOWNLOAD_TIMEOUT = 10;

// --- URL helpers ---

static std::string url_encode(const std::string & s) {
    CURL * curl = curl_easy_init();
    if (!curl) {
        return s;
    }
    char *      enc    = curl_easy_escape(curl, s.c_str(), (int) s.size());
    std::string result = enc ? enc : s;
    curl_free(enc);
    curl_easy_cleanup(curl);
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

struct MetadataHeaders {
    std::string x_repo_commit;
    std::string x_linked_etag;
    std::string x_linked_size;
    std::string etag;
    std::string content_length;
};

static size_t meta_header_cb(char * buf, size_t sz, size_t n, void * ud) {
    auto * h     = static_cast<MetadataHeaders *>(ud);
    size_t total = sz * n;

    std::string line(buf, total);
    auto        colon = line.find(':');
    if (colon == std::string::npos) {
        return total;
    }

    std::string key = line.substr(0, colon);
    std::string val = line.substr(colon + 1);
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
        val.erase(val.begin());
    }
    while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) {
        val.pop_back();
    }

    std::string lk(key.size(), '\0');
    std::transform(key.begin(), key.end(), lk.begin(), ::tolower);

    if (lk == "x-repo-commit") {
        h->x_repo_commit = val;
    } else if (lk == "x-linked-etag") {
        h->x_linked_etag = val;
    } else if (lk == "x-linked-size") {
        h->x_linked_size = val;
    } else if (lk == "etag") {
        h->etag = val;
    } else if (lk == "content-length") {
        h->content_length = val;
    }

    return total;
}

static size_t discard_body(char *, size_t sz, size_t n, void *) {
    return sz * n;
}

struct Metadata {
    std::string url_to_download;
    std::string etag;
    std::string commit_hash;
    long long   size;
    bool        strip_auth;
};

static Metadata get_metadata(const std::string & url, const std::string & token) {
    CURL * curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to init curl");
    }

    MetadataHeaders hdrs;

    struct curl_slist * req_hdrs = nullptr;
    req_hdrs                     = curl_slist_append(req_hdrs, "User-Agent: hf_hub_download/standalone-cpp");
    req_hdrs                     = curl_slist_append(req_hdrs, "Accept-Encoding: identity");
    if (!token.empty()) {
        req_hdrs = curl_slist_append(req_hdrs, ("Authorization: Bearer " + token).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, ETAG_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, req_hdrs);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, meta_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    char * eff_url = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff_url);
    std::string effective_url = eff_url ? eff_url : url;

    curl_slist_free_all(req_hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("HEAD request failed: ") + curl_easy_strerror(res));
    }
    if (http_code >= 400) {
        throw std::runtime_error("HTTP " + std::to_string(http_code) + " for " + url);
    }

    Metadata meta;

    meta.commit_hash = hdrs.x_repo_commit;
    if (meta.commit_hash.empty()) {
        throw std::runtime_error("Server did not return X-Repo-Commit header.");
    }

    std::string raw_etag = hdrs.x_linked_etag.empty() ? hdrs.etag : hdrs.x_linked_etag;
    if (raw_etag.empty()) {
        throw std::runtime_error("Server did not return an ETag header.");
    }
    meta.etag = normalize_etag(raw_etag);

    std::string size_str = hdrs.x_linked_size.empty() ? hdrs.content_length : hdrs.x_linked_size;
    if (size_str.empty()) {
        throw std::runtime_error("Server did not return Content-Length.");
    }
    meta.size = std::stoll(size_str);

    meta.url_to_download = effective_url;
    meta.strip_auth      = (url != effective_url && extract_host(url) != extract_host(effective_url));

    return meta;
}

// --- Streaming GET with resume and progress ---

struct DownloadState {
    FILE *      fp;
    long long   resume_size;
    long long   expected_size;
    int         http_status;
    bool        truncated;
    std::string filename;
};

static size_t dl_header_cb(char * buf, size_t sz, size_t n, void * ud) {
    auto *      s     = static_cast<DownloadState *>(ud);
    size_t      total = sz * n;
    std::string line(buf, total);
    if (line.compare(0, 5, "HTTP/") == 0) {
        auto sp = line.find(' ');
        if (sp != std::string::npos) {
            s->http_status = std::atoi(line.c_str() + sp + 1);
        }
    }
    return total;
}

static size_t dl_write_cb(char * ptr, size_t sz, size_t n, void * ud) {
    auto * s     = static_cast<DownloadState *>(ud);
    size_t total = sz * n;
    // Server returned 200 (full content) instead of 206 — restart from scratch
    if (s->resume_size > 0 && s->http_status == 200 && !s->truncated) {
        fseek(s->fp, 0, SEEK_SET);
        ftruncate(fileno(s->fp), 0);
        s->resume_size = 0;
        s->truncated   = true;
    }
    return fwrite(ptr, 1, total, s->fp);
}

static int dl_progress_cb(void * ud, curl_off_t /*dltotal*/, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto *    s     = static_cast<DownloadState *>(ud);
    long long total = s->expected_size;
    long long now   = dlnow + s->resume_size;
    if (total <= 0) {
        return 0;
    }

    double frac   = std::min((double) now / total, 1.0);
    int    width  = 40;
    int    filled = (int) (frac * width);

    std::string fname = s->filename;
    if (fname.size() > 40) {
        fname = "(…)" + fname.substr(fname.size() - 37);
    }

    char bar[41];
    memset(bar, '#', filled);
    memset(bar + filled, '-', width - filled);
    bar[width] = '\0';

    fprintf(stderr, "\r%-40s [%s] %s / %s  ", fname.c_str(), bar, format_size(now).c_str(), format_size(total).c_str());
    fflush(stderr);
    return 0;
}

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

    DownloadState state{ fp, resume_size, expected_size, 0, false, filename };

    CURL * curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        throw std::runtime_error("Failed to init curl");
    }

    struct curl_slist * hdrs = nullptr;
    hdrs                     = curl_slist_append(hdrs, "User-Agent: hf_hub_download/standalone-cpp");
    if (!token.empty()) {
        hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + token).c_str());
    }
    if (resume_size > 0) {
        hdrs = curl_slist_append(hdrs, ("Range: bytes=" + std::to_string(resume_size) + "-").c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, dl_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, dl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, DOWNLOAD_TIMEOUT);

    CURLcode res       = curl_easy_perform(curl);
    long     http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    fclose(fp);

    fprintf(stderr, "\n");

    if (res != CURLE_OK) {
        if (retries > 0 &&
            (res == CURLE_OPERATION_TIMEDOUT || res == CURLE_COULDNT_CONNECT || res == CURLE_PARTIAL_FILE)) {
            fprintf(stderr, "Retrying (%d left)...\n", retries - 1);
            http_get(url, temp_path, token, expected_size, filename, false, retries - 1);
            return;
        }
        throw std::runtime_error(std::string("Download failed: ") + curl_easy_strerror(res));
    }
    if (http_code >= 400) {
        throw std::runtime_error("HTTP " + std::to_string(http_code) + " downloading " + url);
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

    curl_global_init(CURL_GLOBAL_DEFAULT);

    try {
        std::string path = hf_hub_download(repo_id, filename, repo_type, revision, token, force);
        std::cout << path << "\n";
    } catch (const std::exception & e) {
        fprintf(stderr, "Error: %s\n", e.what());
        curl_global_cleanup();
        return 1;
    }

    curl_global_cleanup();
    return 0;
}
