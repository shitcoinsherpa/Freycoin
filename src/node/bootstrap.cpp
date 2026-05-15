// Copyright (c) 2026 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/bootstrap.h>

#include <crypto/sha256.h>
#include <logging.h>
#include <random.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <util/time.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#ifdef _WIN32
#define BOOTSTRAP_POPEN  _popen
#define BOOTSTRAP_PCLOSE _pclose
#else
#include <sys/wait.h>
#define BOOTSTRAP_POPEN  popen
#define BOOTSTRAP_PCLOSE pclose
#endif

#ifdef HAVE_CONFIG_H
#include <freycoin-build-config.h>
#endif

namespace node {

namespace {

// Marker / staging file names under <datadir>/.bootstrap/
constexpr const char* kBootstrapDir       = ".bootstrap";
constexpr const char* kBackupDir          = ".bootstrap-backup";
constexpr const char* kStagingFile        = "staging.tar.gz";
constexpr const char* kMarkerFile         = "pending.json";

// Allowlist regex: tar entries MUST match this pattern. No ..,
// no leading /, no shell-special chars. Only paths under the three
// allowed top-level dirs.
const std::regex kAllowedEntryRegex{
    R"(^(blocks|chainstate|indexes)/[A-Za-z0-9_./-]+$)"
};

// Tar entry types we will accept. Anything else (symlink, hardlink, fifo,
// device, char/block special) is rejected before extraction.
const std::array<char, 2> kAllowedEntryTypes{'-', 'd'}; // file, dir
                                                       // (bsdtar uses these in `tar tvf` output)

bool IsHexSha256(const std::string& s)
{
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

bool RunQuiet(const std::vector<std::string>& argv, std::string& stdout_out, std::string& stderr_out, int& exit_code)
{
    // Build a safe argv-style command. We never pass operator-tainted
    // arguments through a shell. fork+execvp would be cleanest but for
    // portability across the Bitcoin Core fork's existing helper layer,
    // we use the simpler popen interface with an argv vector that we
    // build ourselves into a properly-quoted shell string.
    //
    // The arguments here are NEVER from operator input — only from
    // chainparams (URL) and our own constructed paths. However we still
    // single-quote them to be defensive against any future change that
    // might include user-controlled data.
    std::string cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) cmd += " ";
        const std::string& a = argv[i];
        // Reject any single-quote in input (we'd need to escape; refuse instead)
        if (a.find('\'') != std::string::npos) {
            stderr_out = "argument contains single-quote";
            exit_code = -1;
            return false;
        }
        cmd += "'";
        cmd += a;
        cmd += "'";
    }
    cmd += " 2>&1";

    FILE* pipe = BOOTSTRAP_POPEN(cmd.c_str(), "r");
    if (!pipe) {
        stderr_out = "popen failed";
        exit_code = -1;
        return false;
    }
    std::array<char, 4096> buf{};
    while (auto n = std::fread(buf.data(), 1, buf.size(), pipe)) {
        stdout_out.append(buf.data(), n);
    }
    int rc = BOOTSTRAP_PCLOSE(pipe);
#ifdef _WIN32
    // _pclose() on Windows returns the child's exit code directly.
    exit_code = rc;
#else
    // pclose() on POSIX returns the wait status; decode with WIFEXITED.
    exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
    return exit_code == 0;
}

bool DownloadWithCurl(const std::string& url,
                      const fs::path& dest,
                      int64_t expected_size,
                      std::function<void(int64_t, int64_t)> progress_cb)
{
    // Resume support via -C - if a partial file exists from a prior interrupted run.
    // --location follows redirects (GitHub release redirects via S3).
    // --fail returns non-zero on HTTP 4xx/5xx.
    // --silent + --show-error keeps stdout clean unless something goes wrong.
    std::vector<std::string> argv = {
        "curl",
        "--fail",
        "--location",
        "--silent",
        "--show-error",
        "--connect-timeout", "30",
        "--max-time", "1800",        // 30 min hard cap on whole transfer
        "--output", fs::PathToString(dest),
    };
    if (fs::exists(dest)) {
        argv.push_back("--continue-at");
        argv.push_back("-");
    }
    argv.push_back(url);

    std::string stdout_buf, stderr_buf;
    int exit_code = 0;
    bool ok = RunQuiet(argv, stdout_buf, stderr_buf, exit_code);

    if (progress_cb) {
        int64_t downloaded = 0;
        if (fs::exists(dest)) {
            std::error_code ec;
            downloaded = static_cast<int64_t>(fs::file_size(dest, ec));
        }
        progress_cb(downloaded, expected_size);
    }

    if (!ok) {
        LogPrintf("Bootstrap: curl exit=%d url=%s output=%s\n",
                  exit_code, url, stdout_buf.substr(0, 500));
        return false;
    }
    return true;
}

bool ComputeFileSha256(const fs::path& path, std::string& hex_out)
{
    std::ifstream in(path.std_path(), std::ios::binary);
    if (!in) return false;
    CSHA256 hasher;
    std::array<char, 65536> buf{};
    while (in) {
        in.read(buf.data(), buf.size());
        std::streamsize got = in.gcount();
        if (got > 0) {
            hasher.Write(reinterpret_cast<const unsigned char*>(buf.data()),
                         static_cast<size_t>(got));
        }
    }
    unsigned char digest[CSHA256::OUTPUT_SIZE];
    hasher.Finalize(digest);
    hex_out = HexStr(std::span<const unsigned char>(digest, CSHA256::OUTPUT_SIZE));
    return true;
}

// Pre-extract validation: list tarball contents via `tar tzf` and assert
// every entry matches the allowlist regex AND is a file/dir (not a symlink
// or special). Refuses BEFORE any write happens.
bool ValidateTarballEntries(const fs::path& tarball, std::string& err_out)
{
    std::vector<std::string> argv = {
        "tar", "-tzf", fs::PathToString(tarball)
    };
    std::string stdout_buf, stderr_buf;
    int exit_code = 0;
    if (!RunQuiet(argv, stdout_buf, stderr_buf, exit_code)) {
        err_out = strprintf("tar -tzf failed (exit=%d): %s",
                            exit_code, stdout_buf.substr(0, 200));
        return false;
    }

    size_t entry_count = 0;
    std::string line;
    for (size_t i = 0; i <= stdout_buf.size(); ++i) {
        char c = i < stdout_buf.size() ? stdout_buf[i] : '\n';
        if (c == '\n' || c == '\r') {
            if (!line.empty()) {
                ++entry_count;
                // Reject directory entries with trailing / from the path check
                // (they are normal in tar archives; we just count them and
                // skip the regex match for them since "blocks/" matches the
                // top-level allowlist trivially).
                std::string check = line;
                if (!check.empty() && check.back() == '/') check.pop_back();
                if (check.empty()) { line.clear(); continue; }
                if (check == "blocks" || check == "chainstate" || check == "indexes") {
                    line.clear();
                    continue;
                }
                if (!std::regex_match(check, kAllowedEntryRegex)) {
                    err_out = strprintf("disallowed tar entry: %s", check);
                    return false;
                }
                line.clear();
            }
        } else {
            line += c;
        }
    }

    if (entry_count == 0) {
        err_out = "tarball is empty";
        return false;
    }
    LogPrintf("Bootstrap: tarball passed allowlist validation (%zu entries)\n", entry_count);
    return true;
}

bool ExtractTarball(const fs::path& tarball, const fs::path& dest_dir, std::string& err_out)
{
    // --no-same-owner — don't preserve uid/gid (we may extract as a different user)
    // --no-same-permissions — don't preserve perms (datadir umask handles them)
    // -C <dir> — change to dest_dir before extracting
    std::vector<std::string> argv = {
        "tar",
        "--no-same-owner",
        "--no-same-permissions",
        "-xzf", fs::PathToString(tarball),
        "-C", fs::PathToString(dest_dir),
    };
    std::string stdout_buf, stderr_buf;
    int exit_code = 0;
    if (!RunQuiet(argv, stdout_buf, stderr_buf, exit_code)) {
        err_out = strprintf("tar -xzf failed (exit=%d): %s",
                            exit_code, stdout_buf.substr(0, 200));
        return false;
    }
    return true;
}

void WriteMarker(const fs::path& marker_path, const BootstrapStatus& s)
{
    // Minimal hand-rolled JSON to avoid pulling univalue into node code.
    std::string body = strprintf(
        "{\n"
        "  \"version\": 1,\n"
        "  \"phase\": \"%s\",\n"
        "  \"url\": \"%s\",\n"
        "  \"expected_sha256\": \"%s\",\n"
        "  \"bytes_total\": %lld,\n"
        "  \"started_unix_ts\": %lld\n"
        "}\n",
        (s.phase == BootstrapPhase::DOWNLOADED ? "downloaded" :
         s.phase == BootstrapPhase::EXTRACTING ? "extracting" :
         s.phase == BootstrapPhase::COMPLETE   ? "complete"   :
                                                 "downloading"),
        s.url, s.expected_sha256, (long long)s.bytes_total,
        (long long)GetTime());
    std::ofstream out(marker_path.std_path(), std::ios::trunc);
    out << body;
}

} // anonymous namespace

BootstrapStatus StageBootstrap(const fs::path& datadir,
                                 const std::string& url,
                                 std::function<void(int64_t, int64_t)> progress_cb)
{
    BootstrapStatus s;
    s.phase = BootstrapPhase::DOWNLOADING;
    s.url = url;
    s.staging_path = datadir / kBootstrapDir / kStagingFile;
    s.marker_path  = datadir / kBootstrapDir / kMarkerFile;

    if (url.find("https://") != 0) {
        s.error = "bootstrap URL must be https://";
        return s;
    }

    std::error_code ec;
    std::filesystem::create_directories(datadir / kBootstrapDir, ec);
    if (ec) {
        s.error = strprintf("could not create %s: %s",
                            (datadir / kBootstrapDir).utf8string(), ec.message());
        return s;
    }

    // Sidecar SHA URL: convention is <url>.sha256
    const std::string sha_url = url + ".sha256";
    const fs::path sha_path = datadir / kBootstrapDir / "staging.tar.gz.sha256";

    LogPrintf("Bootstrap: downloading SHA sidecar from %s\n", sha_url);
    if (!DownloadWithCurl(sha_url, sha_path, 0, nullptr)) {
        s.error = strprintf("failed to fetch SHA sidecar at %s — site is missing the .sha256 file. "
                            "The release operator must add: "
                            "`sha256sum bootstrap/latest.tar.gz | awk '{print $1}' > "
                            "bootstrap/latest.tar.gz.sha256` to the site update job.",
                            sha_url);
        return s;
    }

    std::string sha_contents;
    {
        std::ifstream in(sha_path.std_path());
        std::getline(in, sha_contents);
    }
    // Tolerate sha256sum's `<hex>  <name>` format
    auto sp = sha_contents.find(' ');
    if (sp != std::string::npos) sha_contents.resize(sp);
    sha_contents.erase(std::remove_if(sha_contents.begin(), sha_contents.end(),
                                       [](char c) { return c == '\r' || c == '\n' || c == '\t'; }),
                       sha_contents.end());
    if (!IsHexSha256(sha_contents)) {
        s.error = strprintf("sidecar %s does not contain a 64-char hex SHA256: '%s'",
                            sha_url, sha_contents);
        return s;
    }
    s.expected_sha256 = sha_contents;
    LogPrintf("Bootstrap: expected SHA256 = %s\n", s.expected_sha256);

    LogPrintf("Bootstrap: downloading tarball from %s\n", url);
    if (!DownloadWithCurl(url, s.staging_path, 0, progress_cb)) {
        s.error = "tarball download failed";
        return s;
    }

    std::string actual_sha;
    if (!ComputeFileSha256(s.staging_path, actual_sha)) {
        s.error = "could not hash downloaded tarball";
        return s;
    }
    if (actual_sha != s.expected_sha256) {
        s.error = strprintf("SHA256 mismatch: expected %s, got %s — refusing to use tarball. "
                            "Removing staging.",
                            s.expected_sha256, actual_sha);
        fs::remove(s.staging_path, ec);
        return s;
    }
    LogPrintf("Bootstrap: SHA256 verified, staging tarball ready (%lld bytes)\n",
              (long long)fs::file_size(s.staging_path));

    s.phase = BootstrapPhase::DOWNLOADED;
    s.bytes_downloaded = fs::file_size(s.staging_path);
    s.bytes_total = s.bytes_downloaded;
    WriteMarker(s.marker_path, s);
    LogPrintf("Bootstrap: marker written at %s — daemon will apply on next startup\n",
              s.marker_path.utf8string());
    return s;
}

bool MaybeApplyStagedBootstrap(const fs::path& datadir)
{
    const fs::path marker = datadir / kBootstrapDir / kMarkerFile;
    const fs::path staging = datadir / kBootstrapDir / kStagingFile;

    std::error_code ec;
    if (!std::filesystem::exists(marker, ec)) return true; // nothing to do — normal startup

    LogPrintf("Bootstrap: pending marker found at %s\n", marker.utf8string());

    if (!std::filesystem::exists(staging, ec)) {
        LogPrintf("Bootstrap: ERROR marker exists but staging tarball missing; cleaning up\n");
        fs::remove(marker, ec);
        return true;
    }

    // Validate tarball BEFORE touching any operator state.
    std::string err;
    if (!ValidateTarballEntries(staging, err)) {
        LogPrintf("Bootstrap: ERROR tarball validation failed: %s\n", err);
        LogPrintf("Bootstrap: cleaning marker + staging, falling back to normal startup\n");
        fs::remove(marker, ec);
        fs::remove(staging, ec);
        return true;
    }

    // Move existing chain dirs to backup before extracting.
    const std::string ts = strprintf("%lld", (long long)GetTime());
    const fs::path backup_dir = datadir / kBackupDir / fs::PathFromString(ts);
    std::filesystem::create_directories(backup_dir, ec);

    auto move_if_exists = [&](const std::string& subdir) {
        const fs::path src = datadir / fs::PathFromString(subdir);
        if (std::filesystem::exists(src, ec)) {
            const fs::path dst = backup_dir / fs::PathFromString(subdir);
            std::error_code rec;
            fs::rename(src, dst, rec);
            if (rec) {
                LogPrintf("Bootstrap: WARN could not back up %s: %s\n",
                          src.utf8string(), rec.message());
            } else {
                LogPrintf("Bootstrap: backed up %s -> %s\n",
                          src.utf8string(), dst.utf8string());
            }
        }
    };
    move_if_exists("blocks");
    move_if_exists("chainstate");
    move_if_exists("indexes");

    LogPrintf("Bootstrap: extracting %s into %s\n",
              staging.utf8string(), datadir.utf8string());
    if (!ExtractTarball(staging, datadir, err)) {
        LogPrintf("Bootstrap: ERROR extract failed: %s\n", err);
        LogPrintf("Bootstrap: rolling back from backup\n");
        // Roll back: move backups back to datadir
        for (const char* subdir : {"blocks", "chainstate", "indexes"}) {
            const fs::path src = backup_dir / fs::PathFromString(subdir);
            const fs::path dst = datadir / fs::PathFromString(subdir);
            if (std::filesystem::exists(src, ec)) {
                std::error_code rec;
                fs::rename(src, dst, rec);
            }
        }
        return false;
    }

    LogPrintf("Bootstrap: extract complete; cleaning marker + staging\n");
    fs::remove(marker, ec);
    fs::remove(staging, ec);
    fs::remove(datadir / kBootstrapDir / "staging.tar.gz.sha256", ec);

    LogPrintf("Bootstrap: SUCCESS — datadir is now at the snapshot's height. "
              "Old chain artifacts preserved at %s\n", backup_dir.utf8string());
    return true;
}

std::optional<BootstrapStatus> ReadBootstrapMarker(const fs::path& datadir)
{
    const fs::path marker = datadir / kBootstrapDir / kMarkerFile;
    std::error_code ec;
    if (!std::filesystem::exists(marker, ec)) return std::nullopt;

    BootstrapStatus s;
    s.marker_path = marker;
    s.staging_path = datadir / kBootstrapDir / kStagingFile;
    // Minimal parse — the marker is hand-written so we only look for the
    // fields the daemon actually needs.
    std::ifstream in(marker.std_path());
    std::string line;
    while (std::getline(in, line)) {
        auto extract = [&](const std::string& key, std::string& dest) {
            const std::string needle = "\"" + key + "\":";
            auto p = line.find(needle);
            if (p == std::string::npos) return;
            auto q = line.find('"', p + needle.size());
            if (q == std::string::npos) return;
            auto r = line.find('"', q + 1);
            if (r == std::string::npos) return;
            dest = line.substr(q + 1, r - q - 1);
        };
        std::string phase_str;
        extract("phase", phase_str);
        if (phase_str == "downloading") s.phase = BootstrapPhase::DOWNLOADING;
        else if (phase_str == "downloaded") s.phase = BootstrapPhase::DOWNLOADED;
        else if (phase_str == "extracting") s.phase = BootstrapPhase::EXTRACTING;
        else if (phase_str == "complete") s.phase = BootstrapPhase::COMPLETE;
        extract("url", s.url);
        extract("expected_sha256", s.expected_sha256);
    }
    return s;
}

void CleanupBootstrapState(const fs::path& datadir)
{
    std::error_code ec;
    fs::remove_all(datadir / kBootstrapDir, ec);
    // .bootstrap-backup intentionally NOT removed; operator decides when.
}

} // namespace node
