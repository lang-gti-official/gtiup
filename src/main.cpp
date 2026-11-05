#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <toml.hpp>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#else
#include <openssl/evp.h>
#endif
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <poll.h>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Error = std::runtime_error;
constexpr std::uint64_t download_limit = 1024ULL * 1024 * 1024;
constexpr std::uint64_t archive_limit = 4 * download_limit;
const std::regex version_pattern(
    R"(^v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-beta\.([1-9][0-9]*))?$)");
const std::regex sha_pattern("^[0-9a-f]{40}$");
const std::regex digest_pattern("^[0-9a-f]{64}$");
std::string env(const char *name) {
  auto p = std::getenv(name);
  return p ? p : "";
}
std::string trim(std::string s) {
  auto first = s.find_first_not_of(" \n\r\t");
  return first == s.npos
             ? ""
             : s.substr(first, s.find_last_not_of(" \n\r\t") - first + 1);
}
std::string read(const fs::path &p, std::uint64_t limit = 8 * 1024 * 1024) {
  if (fs::file_size(p) > limit)
    throw Error("file exceeds size limit: " + p.string());
  std::ifstream in(p, std::ios::binary);
  if (!in)
    throw Error("cannot read " + p.string());
  return std::string(std::istreambuf_iterator<char>(in), {});
}
bool contains(const fs::path &root, const fs::path &path) {
  auto a = root.begin(), b = path.begin();
  for (; a != root.end(); ++a, ++b)
    if (b == path.end() || *a != *b)
      return false;
  return true;
}
const std::regex link_pattern("^[A-Za-z][A-Za-z0-9_.-]{0,63}$");
bool is_channel(const std::string &s) {
  return s == "preview" || s == "nightly" || s == "beta" || s == "stable";
}
// A linked toolchain name never collides with a channel, a version or `none`.
bool linked_name(const std::string &s) {
  return std::regex_match(s, link_pattern) && !is_channel(s) && s != "none" &&
         !std::regex_match(s, version_pattern);
}
// Published channels and versions only: what install/update can download.
std::string release_selection(const std::string &s) {
  if (is_channel(s))
    return s;
  if (!std::regex_match(s, version_pattern))
    throw Error("expected nightly, beta, stable, or X.Y.Z[-beta.N]");
  return s[0] == 'v' ? s : "v" + s;
}
// Anything a command or project can select: a release or a linked toolchain.
std::string selection(const std::string &s) {
  if (linked_name(s))
    return s;
  if (!is_channel(s) && !std::regex_match(s, version_pattern))
    throw Error("expected nightly, beta, stable, X.Y.Z[-beta.N], or a linked "
                "toolchain name");
  return release_selection(s);
}
const char *preview_retired =
    "the preview channel is retired and no longer published; use nightly "
    "instead (gtiup install nightly). An installed preview keeps working "
    "until you uninstall it";
// The releases gtiup will download. Preview is retired: refuse it clearly
// rather than silently substituting another channel.
std::string installable(const std::string &s) {
  if (linked_name(s))
    throw Error(s + " is not a release channel; linked toolchains are "
                    "registered with gtiup link and never installed or "
                    "updated by gtiup");
  auto channel = release_selection(s);
  if (channel == "preview")
    throw Error(preview_retired);
  return channel;
}
bool rolling(const std::string &s) { return s == "preview" || s == "nightly"; }
std::string project_selection(const fs::path &path, bool legacy) {
  try {
    const auto contents = read(path, 64 * 1024);
    if (legacy) {
      // Keep the original one-line format, and accept TOML in either filename.
      const auto name = trim(contents);
      if (is_channel(name) || linked_name(name) ||
          std::regex_match(name, version_pattern))
        return selection(name);
    }
    const auto document = toml::parse(contents);
    for (const auto &[key, value] : document) {
      (void)value;
      if (key.str() != "toolchain")
        throw Error("unsupported top-level key: " + std::string(key.str()));
    }
    const auto *toolchain = document["toolchain"].as_table();
    if (!toolchain)
      throw Error("expected a [toolchain] table with a string channel");
    for (const auto &[key, value] : *toolchain) {
      (void)value;
      if (key.str() != "channel")
        throw Error("unsupported toolchain setting: " + std::string(key.str()));
    }
    const auto channel = (*toolchain)["channel"].value<std::string>();
    if (!channel)
      throw Error("toolchain.channel must be a string");
    return selection(*channel);
  } catch (const toml::parse_error &error) {
    throw Error(path.string() + ": invalid TOML: " +
                std::string(error.description()));
  } catch (const std::exception &error) {
    throw Error(path.string() + ": " + error.what());
  }
}
std::array<unsigned long long, 4> version_key(const std::string &s) {
  std::smatch m;
  if (!std::regex_match(s, m, version_pattern))
    throw Error("invalid version");
  return {std::stoull(m[1]), std::stoull(m[2]), std::stoull(m[3]),
          m[5].matched ? std::stoull(m[5]) : 0};
}
std::string host() {
#if defined(__APPLE__) && defined(__aarch64__)
  return "darwin-arm64";
#elif defined(__linux__) && defined(__x86_64__)
  return "linux-x64";
#else
  throw Error("supported toolchain hosts: macOS arm64 and Linux x64");
#endif
}
fs::path executable() {
#ifdef __APPLE__
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> bytes(size);
  if (_NSGetExecutablePath(bytes.data(), &size))
    throw Error("cannot locate gtiup executable");
  return fs::canonical(bytes.data());
#else
  return fs::canonical("/proc/self/exe");
#endif
}
struct Temp {
  fs::path path;
  explicit Temp(const fs::path &parent) {
    fs::create_directories(parent);
    std::string pattern = (parent / ".gtiup-XXXXXX").string();
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back(0);
    if (!mkdtemp(bytes.data()))
      throw Error("cannot create staging directory");
    path = bytes.data();
  }
  ~Temp() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
  Temp(const Temp &) = delete;
};
struct Lock {
  int fd;
  explicit Lock(const fs::path &home) {
    fs::create_directories(home);
    fd = open((home / ".lock").c_str(),
              O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
      throw Error("cannot open manager lock");
    if (flock(fd, LOCK_EX)) {
      close(fd);
      throw Error("cannot lock manager");
    }
  }
  ~Lock() { close(fd); }
};
void atomic_write(const fs::path &p, const std::string &text) {
  Temp tmp(p.parent_path());
  auto staged = tmp.path / "settings";
  int fd = open(staged.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0)
    throw Error("cannot stage settings");
  std::size_t written = 0;
  while (written < text.size()) {
    auto n = write(fd, text.data() + written, text.size() - written);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      close(fd);
      throw Error("cannot write settings");
    }
    written += static_cast<std::size_t>(n);
  }
  if (fsync(fd)) {
    close(fd);
    throw Error("cannot flush settings");
  }
  close(fd);
  fs::rename(staged, p);
}
std::string sha256(const fs::path &p) {
  std::ifstream input(p, std::ios::binary);
  if (!input)
    throw Error("cannot hash " + p.string());
#ifdef __APPLE__
  CC_SHA256_CTX ctx;
  CC_SHA256_Init(&ctx);
#else
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                              EVP_MD_CTX_free);
  if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
    throw Error("SHA256 initialization failed");
#endif
  std::array<char, 65536> buffer;
  while (input) {
    input.read(buffer.data(), buffer.size());
    auto count = input.gcount();
#ifdef __APPLE__
    CC_SHA256_Update(&ctx, buffer.data(), static_cast<CC_LONG>(count));
#else
    if (EVP_DigestUpdate(ctx.get(), buffer.data(),
                         static_cast<std::size_t>(count)) != 1)
      throw Error("SHA256 failed");
#endif
  }
  if (!input.eof())
    throw Error("read failed while hashing");
  unsigned char digest[32];
#ifdef __APPLE__
  CC_SHA256_Final(digest, &ctx);
#else
  unsigned int size = 0;
  if (EVP_DigestFinal_ex(ctx.get(), digest, &size) != 1 || size != 32)
    throw Error("SHA256 finalization failed");
#endif
  const char *hex = "0123456789abcdef";
  std::string result;
  for (auto c : digest) {
    result += hex[c >> 4];
    result += hex[c & 15];
  }
  return result;
}
struct Endpoints {
  std::string manager_api = "https://api.github.com/repos/lang-gti-official/gtiup";
  std::string manager_downloads =
      "https://github.com/lang-gti-official/gtiup/releases/download/";
  std::string api = "https://api.github.com/repos/lang-gti-official/gti";
  std::string downloads =
      "https://github.com/lang-gti-official/gti/releases/download/";
  std::string status = "https://raw.githubusercontent.com/lang-gti-official/"
                       "gti/release-status/release-status.json";
  bool test = false;
  Endpoints() {
#ifdef GTIUP_TESTING
    auto server = env("GTIUP_TEST_SERVER");
    if (!server.empty()) {
      if (!std::regex_match(server,
                            std::regex(R"(^http://127\.0\.0\.1:[0-9]+$)")))
        throw Error("test server must be loopback");
      api = server + "/api";
      manager_api = server + "/manager-api";
      manager_downloads = server + "/manager-downloads/";
      downloads = server + "/downloads/";
      status = server + "/status";
      test = true;
    }
#endif
  }
};
struct Download {
  std::ofstream out;
  std::uint64_t size = 0, limit;
  Download(const fs::path &p, std::uint64_t maximum)
      : out(p, std::ios::binary), limit(maximum) {}
};
size_t receive(char *ptr, size_t size, size_t count, void *data) {
  auto &sink = *static_cast<Download *>(data);
  if (size && count > SIZE_MAX / size)
    return 0;
  auto bytes = size * count;
  if (bytes > sink.limit - sink.size)
    return 0;
  sink.out.write(ptr, static_cast<std::streamsize>(bytes));
  if (!sink.out)
    return 0;
  sink.size += bytes;
  return bytes;
}
void status(const std::string &message) {
  std::cerr << "gtiup: " << message << '\n';
}
struct DownloadProgress {
  bool terminal;
  bool line = false;
  std::size_t width = 0;
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point updated = started;

  static int report(void *data, curl_off_t total, curl_off_t received,
                    curl_off_t, curl_off_t) noexcept {
    auto &progress = *static_cast<DownloadProgress *>(data);
    if (!progress.terminal)
      return 0;
    try {
      const auto now = std::chrono::steady_clock::now();
      if (progress.line && now - progress.updated < std::chrono::seconds(1))
        return 0;
      progress.updated = now;
      std::ostringstream text;
      text << "  " << std::fixed << std::setprecision(1)
           << received / (1024.0 * 1024.0);
      if (total > 0)
        text << " / " << total / (1024.0 * 1024.0) << " MiB ("
             << std::setprecision(0) << std::min(100.0, 100.0 * received / total) << "%)";
      else
        text << " MiB";
      text << " - " << std::chrono::duration_cast<std::chrono::seconds>(now - progress.started).count()
           << "s";
      const auto rendered = text.str();
      progress.width = std::max(progress.width, rendered.size());
      std::cerr << '\r' << rendered << std::string(progress.width - rendered.size(), ' ') << std::flush;
      progress.line = true;
      return 0;
    } catch (...) {
      // Never allow an exception to cross libcurl's C callback boundary.
      return 1;
    }
  }
  void finish() {
    if (line)
      std::cerr << '\n';
  }
};
void download(const std::string &url, const fs::path &target,
              std::uint64_t limit = download_limit, bool test = false,
              const std::string &label = {}) {
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                           curl_easy_cleanup);
  if (!curl)
    throw Error("cannot initialize HTTPS client");
  Download sink(target, limit);
  if (!sink.out)
    throw Error("cannot write download");
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR,
                   test ? "http,https" : "https");
  curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR,
                   test ? "http,https" : "https");
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 600L);
  curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "gtiup/" GTIUP_VERSION);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &sink);
  DownloadProgress progress{!label.empty() && isatty(STDERR_FILENO)};
  if (!label.empty()) {
    status("Downloading " + label + "...");
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, DownloadProgress::report);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &progress);
  }
  auto result = curl_easy_perform(curl.get());
  progress.finish();
  sink.out.close();
  if (result != CURLE_OK || !sink.out)
    throw Error("download failed: " + url + ": " + curl_easy_strerror(result));
  if (!label.empty())
    status("Downloaded " + label + " (" + std::to_string(sink.size) + " bytes)");
}
Json remote_json(const std::string &url, bool test) {
  Temp tmp(fs::temp_directory_path());
  auto p = tmp.path / "metadata";
  download(url, p, 8 * 1024 * 1024, test);
  return Json::parse(read(p));
}
struct Release {
  std::string tag, stem, version;
};
Release release(std::string channel, const Endpoints &urls) {
  channel = release_selection(channel);
  Json status;
  if (!rolling(channel)) {
    status = remote_json(urls.status, urls.test);
    if (status.value("schema", 0) != 1 || !status.at("stable").is_array() ||
        !status.at("yanked").is_array() || !status.at("withdrawn").is_array())
      throw Error("invalid release status");
    for (const auto &v : status["stable"]) {
      auto s = v.get<std::string>();
      if (selection(s) != "v" + s || s.find("-beta.") != s.npos)
        throw Error("invalid stable status");
    }
    for (const auto &entry : status["yanked"]) {
      auto s = entry.at("version").get<std::string>();
      if (selection(s) != "v" + s || s.find("-beta.") != s.npos)
        throw Error("invalid yanked version");
    }
    for (const auto &entry : status["withdrawn"]) {
      auto s = entry.at("tag").get<std::string>();
      if (selection(s) != s || s.find("-beta.") == s.npos)
        throw Error("invalid withdrawn tag");
    }
  }
  if (channel == "stable" || channel == "beta") {
    std::vector<std::string> candidates;
    if (channel == "stable")
      for (const auto &v : status["stable"])
        candidates.push_back(selection(v));
    else
      for (int page = 1;; ++page) {
        if (page > 100)
          throw Error("release pagination limit exceeded");
        auto batch = remote_json(
            urls.api + "/releases?per_page=100&page=" + std::to_string(page),
            urls.test);
        if (!batch.is_array())
          throw Error("invalid release list");
        for (const auto &r : batch) {
          auto tag = r.at("tag_name").get<std::string>();
          if (!r.at("draft").get<bool>() && r.at("prerelease").get<bool>() &&
              std::regex_match(tag, version_pattern) &&
              tag.find("-beta.") != tag.npos)
            candidates.push_back(selection(tag));
        }
        if (batch.size() < 100)
          break;
      }
    if (candidates.empty())
      throw Error("no " + channel +
                  " release published; choose another channel explicitly");
    channel = *std::max_element(candidates.begin(), candidates.end(),
                                [](const auto &a, const auto &b) {
                                  return version_key(a) < version_key(b);
                                });
  }
  std::string version = channel;
  if (channel[0] == 'v')
    version = channel.substr(1, channel.find("-beta.") == channel.npos
                                    ? channel.npos
                                    : channel.find("-beta.") - 1);
  if (!status.is_null()) {
    for (const auto &e : status["yanked"])
      if (e.at("version") == version)
        throw Error(channel + " is yanked; choose another explicitly");
    for (const auto &e : status["withdrawn"])
      if (e.at("tag") == channel)
        throw Error(channel + " is withdrawn; choose another explicitly");
  }
  auto metadata =
      remote_json(urls.api + "/releases/tags/" + channel, urls.test);
  if (metadata.at("draft").get<bool>() || metadata.at("tag_name") != channel)
    throw Error("release identity mismatch");
  bool beta = channel.find("-beta.") != channel.npos;
  if (!rolling(channel) && metadata.at("prerelease").get<bool>() != beta)
    throw Error("release stage mismatch");
  if (!rolling(channel) && !beta &&
      std::find(status["stable"].begin(), status["stable"].end(), version) ==
          status["stable"].end())
    throw Error("stable release not recorded as published");
  return {channel, rolling(channel) ? "gti-" + channel : "gti-v" + version,
          version};
}
struct Reader {
  archive *value = archive_read_new();
  explicit Reader(const fs::path &p) {
    if (!value)
      throw Error("archive reader allocation failed");
    archive_read_support_filter_gzip(value);
    archive_read_support_format_tar(value);
    if (archive_read_open_filename(value, p.c_str(), 65536) != ARCHIVE_OK) {
      std::string error = archive_error_string(value);
      archive_read_free(value);
      value = nullptr;
      throw Error(error);
    }
  }
  ~Reader() {
    if (value)
      archive_read_free(value);
  }
  bool next(archive_entry **e) {
    int code = archive_read_next_header(value, e);
    if (code == ARCHIVE_EOF)
      return false;
    if (code != ARCHIVE_OK)
      throw Error(archive_error_string(value));
    return true;
  }
};
fs::path safe_relative(const char *raw) {
  if (!raw || !*raw)
    throw Error("empty archive path");
  fs::path p(raw);
  if (p.is_absolute())
    throw Error("absolute archive path");
  for (const auto &part : p)
    if (part == "..")
      throw Error("archive path traversal");
  return p.lexically_normal();
}
void extract(const fs::path &package, const fs::path &destination) {
  std::set<fs::path> paths, links;
  std::uint64_t total = 0;
  {
    Reader reader(package);
    archive_entry *e;
    while (reader.next(&e)) {
      auto p = safe_relative(archive_entry_pathname(e));
      auto type = archive_entry_filetype(e);
      if (archive_entry_hardlink(e) ||
          (type != AE_IFREG && type != AE_IFDIR && type != AE_IFLNK))
        throw Error("unsupported archive entry");
      if (p == ".") {
        if (type != AE_IFDIR)
          throw Error("invalid archive root");
        continue;
      }
      // Treat directory trailing separators as the same pathname.
      while (p.filename().empty())
        p = p.parent_path();
      if (!paths.insert(p).second || paths.size() > 100000)
        throw Error("duplicate or excessive archive entries");
      auto size = archive_entry_size(e);
      if (size < 0 || static_cast<std::uint64_t>(size) > archive_limit - total)
        throw Error("archive exceeds extraction limit");
      total += static_cast<std::uint64_t>(size);
      if (type == AE_IFLNK) {
        safe_relative(archive_entry_symlink(e));
        links.insert(p);
      }
    }
  }
  for (const auto &p : paths)
    for (auto parent = p.parent_path(); !parent.empty();
         parent = parent.parent_path())
      if (links.count(parent))
        throw Error("archive writes through symlink");
  Reader reader(package);
  archive_entry *e;
  while (reader.next(&e)) {
    auto relative = safe_relative(archive_entry_pathname(e));
    if (relative == ".")
      continue;
    auto target = destination / relative;
    fs::create_directories(target.parent_path());
    auto type = archive_entry_filetype(e);
    if (type == AE_IFDIR) {
      fs::create_directories(target);
      continue;
    }
    if (type == AE_IFLNK) {
      fs::create_symlink(archive_entry_symlink(e), target);
      continue;
    }
    int fd = open(target.c_str(),
                  O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
                  archive_entry_perm(e) & 0777);
    if (fd < 0)
      throw Error("cannot create archive member " + target.string());
    std::array<char, 65536> bytes;
    std::uint64_t remaining = static_cast<std::uint64_t>(archive_entry_size(e));
    for (;;) {
      auto n = archive_read_data(reader.value, bytes.data(), bytes.size());
      if (n < 0) {
        close(fd);
        throw Error("invalid archive data");
      }
      if (!n)
        break;
      if (static_cast<std::uint64_t>(n) > remaining) {
        close(fd);
        throw Error("archive size mismatch");
      }
      remaining -= static_cast<std::uint64_t>(n);
      ssize_t offset = 0;
      while (offset < n) {
        auto count = write(fd, bytes.data() + offset,
                           static_cast<std::size_t>(n - offset));
        if (count < 0 && errno == EINTR)
          continue;
        if (count <= 0) {
          close(fd);
          throw Error("archive write failed");
        }
        offset += count;
      }
    }
    close(fd);
    if (remaining)
      throw Error("truncated archive member");
  }
}
// Probe only a checksum-verified candidate, without a shell. Bound both runtime
// and output so a broken release cannot hang an update or fill a filesystem.
std::string manager_version(const fs::path &binary) {
  int output[2];
  if (pipe(output))
    throw Error("cannot create manager version pipe");
  const auto path = binary.string();
  const auto child = fork();
  if (child == 0) {
    close(output[0]);
    dup2(output[1], STDOUT_FILENO);
    dup2(output[1], STDERR_FILENO);
    close(output[1]);
    const int null = open("/dev/null", O_RDONLY);
    if (null >= 0) {
      dup2(null, STDIN_FILENO);
      close(null);
    }
    execl(path.c_str(), path.c_str(), "--version", static_cast<char *>(nullptr));
    _exit(127);
  }
  close(output[1]);
  if (child < 0) {
    close(output[0]);
    throw Error("cannot start manager version check");
  }
  bool reaped = false;
  int status = 0;
  std::string result;
  try {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool eof = false;
    while (!eof || !reaped) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw Error("manager version check timed out");
      pollfd fd{output[0], POLLIN, 0};
      const auto ready = poll(eof ? nullptr : &fd, eof ? 0 : 1, 50);
      if (ready < 0 && errno != EINTR)
        throw Error("cannot read manager version");
      if (ready > 0 && (fd.revents & (POLLIN | POLLHUP))) {
        std::array<char, 256> bytes;
        const auto count = ::read(output[0], bytes.data(), bytes.size());
        if (count == 0)
          eof = true;
        else if (count > 0) {
          result.append(bytes.data(), static_cast<std::size_t>(count));
          if (result.size() > 256)
            throw Error("manager version output exceeds size limit");
        } else if (errno != EINTR)
          throw Error("cannot read manager version");
      }
      if (!reaped) {
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
          reaped = true;
        else if (waited < 0 && errno != EINTR)
          throw Error("cannot wait for manager version check");
      }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      throw Error("downloaded manager failed its version check");
  } catch (...) {
    if (!reaped) {
      kill(child, SIGKILL);
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    close(output[0]);
    throw;
  }
  close(output[0]);
  return trim(result);
}
struct Manager {
  fs::path home;
  Endpoints urls;
  Manager() {
    auto configured = env("GTIUP_HOME");
    if (configured.empty()) {
      auto base = env("HOME");
      if (base.empty())
        throw Error("HOME or GTIUP_HOME must be set");
      home = fs::path(base) / ".gtiup";
    } else
      home = configured;
    home = fs::absolute(home).lexically_normal();
  }
  Json state() const {
    auto p = home / "settings.json";
    if (!fs::exists(p))
      return Json{
          {"schema", 1}, {"default", nullptr}, {"toolchains", Json::object()}};
    auto s = Json::parse(read(p));
    if (s.at("schema") != 1 || !s.at("toolchains").is_object() ||
        (!s.at("default").is_null() && !s.at("default").is_string()))
      throw Error("invalid gtiup settings");
    return s;
  }
  void save(const Json &s) const {
    atomic_write(home / "settings.json", s.dump(2) + "\n");
  }
  std::string selected() const {
    auto override = env("GTIUP_TOOLCHAIN");
    if (!override.empty())
      return selection(override);
    const auto project = [this](const fs::path &file, bool legacy) {
      auto name = project_selection(file, legacy);
      if (linked_name(name) && !state()["toolchains"].contains(name))
        throw Error(file.string() + ": no linked toolchain named " + name +
                    "; register one with gtiup link " + name + " PREFIX");
      return name;
    };
    for (auto p = fs::current_path();; p = p.parent_path()) {
      if (fs::exists(p / "gti-toolchain"))
        return project(p / "gti-toolchain", true);
      if (fs::exists(p / "gti-toolchain.toml"))
        return project(p / "gti-toolchain.toml", false);
      if (p == p.root_path())
        break;
    }
    auto s = state();
    if (s["default"].is_null())
      throw Error("no default toolchain; run gtiup default nightly (or "
                  "beta/stable, or a linked toolchain)");
    return selection(s["default"]);
  }
  fs::path which(const std::string &tool, std::string channel = "") const {
    if (tool != "gti" && tool != "gti_lsp")
      throw Error("tool must be gti or gti_lsp");
    channel = channel.empty() ? selected() : selection(channel);
    auto s = state();
    if (!s["toolchains"].contains(channel)) {
      if (channel == "preview")
        throw Error("preview is retired and not installed; select nightly "
                    "instead (gtiup default nightly)");
      if (linked_name(channel))
        throw Error("no linked toolchain named " + channel +
                    "; register one with gtiup link " + channel + " PREFIX");
      throw Error(channel + " is not installed; run gtiup install " + channel);
    }
    const auto &record = s["toolchains"][channel];
    if (record.contains("linked"))
      return linked_tool(record.at("linked").get<std::string>(), tool, channel);
    auto directory = record.at("directory").get<std::string>();
    if (directory.empty() || fs::path(directory).filename() != directory ||
        directory == "." || directory == "..")
      throw Error("invalid toolchain directory");
    auto root = fs::canonical(home / "toolchains" / directory);
    if (!contains(fs::canonical(home / "toolchains"), root))
      throw Error("toolchain path escapes store");
    auto binary = fs::canonical(root / "bin" / tool);
    if (!contains(root, binary) || !fs::is_regular_file(binary) ||
        access(binary.c_str(), X_OK))
      throw Error("toolchain executable missing");
    return binary;
  }
  // A linked prefix is used where it is, exactly like a managed prefix: the
  // executable must be a regular file inside it so the compiler finds its own
  // share/gti resources. gtiup never writes to or deletes a linked prefix.
  static fs::path linked_tool(const fs::path &prefix, const std::string &tool,
                              const std::string &name) {
    if (!fs::is_directory(prefix))
      throw Error("linked toolchain " + name + " prefix is missing: " +
                  prefix.string());
    const auto root = fs::canonical(prefix);
    const auto path = root / "bin" / tool;
    if (!fs::exists(path))
      throw Error("linked toolchain " + name + " has no bin/" + tool + " in " +
                  prefix.string());
    const auto binary = fs::canonical(path);
    if (!contains(root, binary) || !fs::is_regular_file(binary) ||
        access(binary.c_str(), X_OK))
      throw Error("linked toolchain " + name + ": bin/" + tool +
                  " must be an executable file inside " + prefix.string());
    return binary;
  }
  void link(const std::string &name, const std::string &requested) {
    if (!linked_name(name))
      throw Error("invalid linked toolchain name " + name +
                  ": start with a letter and use letters, digits, '.', '_' or "
                  "'-'; channel and version names are reserved");
    auto prefix = fs::absolute(requested).lexically_normal();
    if (prefix.filename().empty())
      prefix = prefix.parent_path();
    linked_tool(prefix, "gti", name);
    if (fs::exists(prefix / "bin/gti_lsp") || fs::is_symlink(prefix / "bin/gti_lsp"))
      linked_tool(prefix, "gti_lsp", name);
    Lock lock(home);
    auto s = state();
    std::string previous;
    if (s["toolchains"].contains(name))
      previous = s["toolchains"][name].value("linked", "");
    s["toolchains"][name] = Json{{"linked", prefix.string()}};
    save(s);
    std::cout << "Linked " << name << ": " << prefix.string();
    if (!previous.empty() && previous != prefix.string())
      std::cout << " (was " << previous << ")";
    std::cout << '\n';
  }
  void unlink(const std::string &name) {
    Lock lock(home);
    auto s = state();
    if (!s["toolchains"].contains(name) || !s["toolchains"][name].contains("linked"))
      throw Error("no linked toolchain named " + name +
                  (s["toolchains"].contains(name) ? "; use gtiup uninstall " + name : ""));
    if (s["default"] == name)
      throw Error("select another default before unlinking");
    const auto prefix = s["toolchains"][name].at("linked").get<std::string>();
    s["toolchains"].erase(name);
    save(s);
    std::cout << "Unlinked " << name << "; " << prefix << " is unchanged\n";
  }
  // A short, readable name for an installed build: a rolling channel's
  // commit is abbreviated; a versioned release is its version.
  static std::string build_label(const std::string &identity) {
    return std::regex_match(identity, sha_pattern) ? identity.substr(0, 7) : identity;
  }
  static std::string version_note(const std::string &before, const std::string &after) {
    if (after.empty())
      return "";
    if (before.empty() || before == after)
      return " (GTI " + after + ")";
    return " (GTI " + before + " -> " + after + ")";
  }
  // The compiler repository's web URL, for a link to the commits between two
  // rolling builds; empty for any other (test) endpoint.
  std::string compare_url(const std::string &from, const std::string &to) const {
    const std::string suffix = "/releases/download/";
    if (urls.downloads.rfind("https://github.com/", 0) != 0 ||
        urls.downloads.size() <= suffix.size() ||
        urls.downloads.compare(urls.downloads.size() - suffix.size(), suffix.size(), suffix) != 0)
      return "";
    return urls.downloads.substr(0, urls.downloads.size() - suffix.size()) +
           "/compare/" + from + "..." + to;
  }
  void install(const std::string &requested, bool set_default = false) {
    auto channel = installable(requested);
    status("Checking " + channel + " release...");
    Lock lock(home);
    auto rel = release(channel, urls);
    // What this channel had before, so the result can say whether it moved.
    Json previous;
    {
      auto current = state();
      if (current["toolchains"].contains(channel) &&
          current["toolchains"][channel].contains("sha256"))
        previous = current["toolchains"][channel];
    }
    auto asset = rel.stem + "-" + host() + ".tar.gz";
    auto store = home / "toolchains";
    fs::create_directories(store);
    Temp tmp(store);
    auto base = urls.downloads + rel.tag + "/";
    download(base + asset + ".sha256", tmp.path / "checksum", 4096, urls.test);
    std::istringstream line(read(tmp.path / "checksum"));
    std::string expected, filename, extra;
    line >> expected >> filename;
    if (!filename.empty() && filename[0] == '*')
      filename.erase(0, 1);
    if (!std::regex_match(expected, digest_pattern) || filename != asset ||
        (line >> extra))
      throw Error("invalid release checksum");
    // The published archive is the one already installed: report that the
    // channel has not moved instead of downloading it again. A separately
    // updated LSP component is the exception; reinstalling restores the
    // toolchain's own.
    if (!previous.is_null() && previous.value("sha256", "") == expected &&
        previous.value("tag", "") == rel.tag && !previous.contains("lsp") &&
        fs::is_directory(store / previous.value("directory", "")) &&
        !fs::is_symlink(store / previous.value("directory", ""))) {
      if (set_default) {
        auto s = state();
        s["default"] = channel;
        save(s);
      }
      std::cout << channel << " is already up to date: "
                << build_label(previous.value("identity", ""))
                << version_note("", previous.value("version", "")) << '\n';
      return;
    }
    auto package = tmp.path / asset;
    download(base + asset, package, download_limit, urls.test, channel + " (" + host() + ")");
    status("Verifying archive checksum...");
    auto digest = sha256(package);
    if (digest != expected)
      throw Error("archive checksum mismatch");
    auto staged = tmp.path / "stage";
    fs::create_directory(staged);
    status("Extracting " + channel + "...");
    extract(package, staged);
    status("Verifying toolchain identity...");
    for (const auto &tool : {"gti", "gti_lsp"}) {
      auto p = staged / "bin" / tool;
      if (!fs::is_regular_file(p) || access(p.c_str(), X_OK))
        throw Error(std::string("release lacks executable ") + tool);
    }
    auto packaged_version = trim(read(staged / "share/gti/VERSION", 256));
    if (!std::regex_match(packaged_version, version_pattern) ||
        packaged_version[0] == 'v' ||
        packaged_version.find("-beta.") != packaged_version.npos)
      throw Error("invalid packaged VERSION");
    std::string identity = rel.version;
    if (rolling(rel.tag)) {
      download(base + "BUILD_ID", tmp.path / "BUILD_ID", 128, urls.test);
      identity = trim(read(tmp.path / "BUILD_ID"));
      if (!std::regex_match(identity, sha_pattern) ||
          trim(read(staged / "share/gti/BUILD_ID", 128)) != identity)
        throw Error(rel.tag + " changed during download; retry");
    } else if (packaged_version != rel.version)
      throw Error("archive version mismatch");
    auto directory = rel.tag + "-" + digest;
    status("Activating " + channel + "...");
    auto target = store / directory;
    if (fs::is_symlink(target))
      throw Error("toolchain directory must not be a symlink");
    if (!fs::exists(target))
      fs::rename(staged, target);
    auto s = state();
    s["toolchains"][channel] = Json{{"directory", directory},
                                    {"tag", rel.tag},
                                    {"identity", identity},
                                    {"version", packaged_version},
                                    {"sha256", digest}};
    if (set_default)
      s["default"] = channel;
    save(s);
    const auto before = previous.is_null() ? std::string() : previous.value("identity", "");
    const auto before_version = previous.is_null() ? std::string() : previous.value("version", "");
    if (before.empty()) {
      std::cout << "Installed " << channel << ": " << build_label(identity)
                << version_note("", packaged_version) << '\n';
    } else if (before == identity) {
      std::cout << "Reinstalled " << channel << ": " << build_label(identity)
                << (previous.contains("lsp") ? " (same build; restored its bundled LSP)"
                                             : " (same build)")
                << '\n';
    } else {
      std::cout << "Updated " << channel << ": " << build_label(before) << " -> "
                << build_label(identity) << version_note(before_version, packaged_version)
                << '\n';
      const auto link = rolling(rel.tag) && std::regex_match(before, sha_pattern)
                            ? compare_url(before, identity) : std::string();
      if (!link.empty())
        std::cout << "Changes: " << link << '\n';
    }
  }
  void update_lsp(std::string channel) {
    channel = channel.empty() ? selected() : selection(channel);
    status("Checking compatible LSP updates for " + channel + "...");
    Lock lock(home);
    auto s = state();
    if (s["toolchains"].contains(channel) && s["toolchains"][channel].contains("linked"))
      throw Error(channel + " is a linked toolchain; gtiup never updates it. "
                            "Reinstall your build into its prefix instead");
    // which() checks the selected immutable store and executable containment.
    const auto base = which("gti", channel).parent_path().parent_path();
    const auto compatibility_file = base / "share/gti/LSP_COMPATIBILITY.json";
    if (!fs::is_regular_file(compatibility_file))
      throw Error("this toolchain predates independent LSP updates; run gtiup update " + channel);
    const auto contract = Json::parse(read(compatibility_file, 4096));
    const auto compatibility = contract.at("compatibility").get<std::string>();
    if (contract.at("schema") != 1 || !std::regex_match(compatibility, digest_pattern))
      throw Error("invalid toolchain LSP compatibility identity");
    // Preserve revocation checks for installed versioned compiler toolchains.
    const auto compiler_tag = s["toolchains"][channel].at("tag").get<std::string>();
    if (!rolling(compiler_tag))
      release(compiler_tag, urls);
    const auto prefix = "lsp-" + compatibility + "-";
    Json newest;
    std::uint64_t newest_run = 0;
    for (int page = 1;; ++page) {
      if (page > 100)
        throw Error("release pagination limit exceeded");
      const auto batch = remote_json(urls.api + "/releases?per_page=100&page=" + std::to_string(page), urls.test);
      if (!batch.is_array())
        throw Error("invalid release list");
      for (const auto &r : batch) {
        const auto tag = r.at("tag_name").get<std::string>();
        if (r.at("draft").get<bool>() || !r.at("prerelease").get<bool>() || tag.rfind(prefix, 0) != 0)
          continue;
        const auto suffix = tag.substr(prefix.size());
        if (!std::regex_match(suffix, std::regex("[1-9][0-9]{0,19}")))
          throw Error("invalid LSP release ordering");
        const auto run = std::stoull(suffix);
        if (run > newest_run) {
          newest_run = run;
          newest = r;
        }
      }
      if (batch.size() < 100)
        break;
    }
    if (newest.is_null()) {
      std::cout << "No compatible LSP update is published for " << channel << '\n';
      return;
    }
    auto &record = s["toolchains"][channel];
    if (record.contains("lsp") && record["lsp"].at("compatibility") == compatibility &&
        record["lsp"].at("run_id").get<std::uint64_t>() >= newest_run) {
      std::cout << "LSP for " << channel << " is already up to date\n";
      return;
    }
    const auto tag = newest.at("tag_name").get<std::string>();
    const auto url = urls.downloads + tag + "/";
    const auto manifest = remote_json(url + "lsp.json", urls.test);
    const auto source = manifest.at("source").get<std::string>();
    if (manifest.at("schema") != 1 || manifest.at("kind") != "gti-lsp" ||
        manifest.at("compatibility") != compatibility || !std::regex_match(source, sha_pattern) ||
        newest.at("target_commitish") != source || !manifest.at("run_id").is_number_unsigned() ||
        manifest.at("run_id").get<std::uint64_t>() != newest_run ||
        !manifest.at("attempt").is_number_unsigned() || manifest.at("attempt").get<std::uint64_t>() == 0)
      throw Error("LSP release identity or compatibility mismatch");
    const auto installed_source = record.contains("lsp")
        ? record["lsp"].at("source").get<std::string>()
        : trim(read(base / "share/gti/LSP_SOURCE", 128));
    if (!std::regex_match(installed_source, sha_pattern))
      throw Error("installed LSP source identity is unavailable; update the complete toolchain");
    if (installed_source != source) {
      const auto comparison = remote_json(urls.api + "/compare/" + installed_source + "..." + source, urls.test);
      if (comparison.at("status") != "ahead" && comparison.at("status") != "identical")
        throw Error("published LSP is older than or diverges from the installed toolchain; keeping the current LSP");
    }
    const auto platform = host();
    const auto asset = "gti-lsp-" + platform + ".tar.gz";
    const auto item = manifest.at("platforms").at(platform);
    const auto digest = item.at("sha256").get<std::string>();
    if (item.at("asset") != asset || !std::regex_match(digest, digest_pattern))
      throw Error("invalid LSP archive identity");
    Temp tmp(home / "toolchains");
    download(url + asset, tmp.path / asset, download_limit, urls.test, "LSP for " + channel);
    status("Verifying compatible LSP archive...");
    if (sha256(tmp.path / asset) != digest)
      throw Error("LSP archive checksum mismatch");
    const auto component = tmp.path / "component";
    fs::create_directory(component);
    extract(tmp.path / asset, component);
    const auto binary = component / "bin/gti_lsp";
    const auto identity_file = component / "share/gti/LSP_COMPONENT.json";
    const Json identity = {{"schema", 1}, {"kind", "gti-lsp"}, {"compatibility", compatibility},
                           {"source", source}, {"run_id", newest_run}, {"attempt", manifest.at("attempt")},
                           {"platform", platform}};
    for (const auto &entry : fs::recursive_directory_iterator(component)) {
      const auto name = entry.path().lexically_relative(component).generic_string();
      if (entry.is_symlink() || (entry.is_regular_file() && name != "bin/gti_lsp" &&
          name != "share/gti/LSP_COMPONENT.json" && name.rfind("share/licenses/gti/", 0) != 0))
        throw Error("unexpected LSP component contents");
    }
    if (!fs::is_regular_file(binary) || access(binary.c_str(), X_OK) ||
        Json::parse(read(identity_file, 4096)) != identity)
      throw Error("embedded LSP identity or executable mismatch");
    status("Activating compatible LSP for " + channel + "...");
    // Clone the immutable prefix using hard links. Only the LSP inode is
    // replaced; compiler, parser and stdlib bytes retain their original identity.
    // All resources remain at their normal relative paths for both Neovim and
    // direct gti_lsp invocations. Older running processes keep their old prefix.
    const auto staged = tmp.path / "stage";
    for (const auto &entry : fs::recursive_directory_iterator(base))
      if (entry.is_symlink() && !contains(base, fs::canonical(entry.path())))
        throw Error("installed toolchain contains an escaping symlink");
    fs::copy(base, staged, fs::copy_options::recursive | fs::copy_options::create_hard_links |
                                fs::copy_options::copy_symlinks);
    fs::remove(staged / "bin/gti_lsp");
    fs::rename(binary, staged / "bin/gti_lsp");
    atomic_write(staged / "share/gti/LSP_COMPONENT.json", identity.dump(2) + "\n");
    // Include the original prefix identity so reusing one compatible LSP with
    // two toolchains cannot accidentally substitute the other's resources.
    atomic_write(tmp.path / "activation", base.filename().string() + "\n" + digest);
    const auto directory = compiler_tag + "-" + sha256(tmp.path / "activation");
    const auto target = home / "toolchains" / directory;
    if (fs::exists(target) || fs::is_symlink(target))
      throw Error("LSP activation directory already exists; run gtiup gc before retrying");
    fs::rename(staged, target);
    record["directory"] = directory;
    record["lsp"] = identity;
    record["lsp"]["sha256"] = digest;
    record["lsp"]["tag"] = tag;
    save(s);
    std::cout << "Updated LSP for " << channel << ": " << source << '\n';
  }
  void self_update() {
    status("Checking for gtiup updates...");
#ifdef __linux__
    // Another updater may have replaced the inode after this process started.
    // Keep the installed pathname; the lock and recorded digest below validate
    // the current on-disk manager before deciding whether an update is needed.
    auto running = fs::read_symlink("/proc/self/exe").string();
    const std::string deleted = " (deleted)";
    if (running.size() >= deleted.size() &&
        running.compare(running.size() - deleted.size(), deleted.size(), deleted) == 0)
      running.resize(running.size() - deleted.size());
    const auto self = fs::weakly_canonical(running);
#else
    const auto self = executable();
#endif
    Lock lock(home);
    const auto bin = home / "bin";
    const auto destination = bin / "gtiup";
    const auto record = home / "manager.json";
    if (fs::is_symlink(bin) || fs::is_symlink(destination) ||
        !fs::is_regular_file(destination) || fs::canonical(destination) != self)
      throw Error("self update requires the installed gtiup in " + bin.string() +
                  "; install the manager first");
    if (fs::is_symlink(record))
      throw Error("refusing to overwrite manager metadata symlink");
    const auto metadata = Json::parse(read(record, 4096));
    if (metadata.value("schema", 0) != 1 || metadata.at("sha256") != sha256(destination))
      throw Error("installed gtiup is not the recorded manager; refusing update");
    const auto current = metadata.at("version").get<std::string>();
    const std::regex stable_version(R"(^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$)");
    if (!std::regex_match("v" + current, stable_version))
      throw Error("invalid installed manager version");
    const auto latest = remote_json(urls.manager_api + "/releases/latest", urls.test);
    const auto tag = latest.at("tag_name").get<std::string>();
    if (latest.at("draft") != false || latest.at("prerelease") != false ||
        !std::regex_match(tag, stable_version))
      throw Error("invalid stable manager release");
    if (version_key(tag) <= version_key(current)) {
      std::cout << "gtiup " << current << " is already up to date\n";
      return;
    }
    const auto version = tag.substr(1);
    const auto asset = "gtiup-" + host();
    const auto base = urls.manager_downloads + tag + "/";
    Temp tmp(bin);
    download(base + asset + ".sha256", tmp.path / "checksum", 4096, urls.test);
    std::istringstream line(read(tmp.path / "checksum"));
    std::string expected, filename, extra;
    line >> expected >> filename;
    if (!std::regex_match(expected, digest_pattern) || filename != asset || (line >> extra))
      throw Error("invalid manager checksum");
    const auto candidate = tmp.path / "gtiup";
    download(base + asset, candidate, 64 * 1024 * 1024, urls.test, "gtiup " + version + " (" + host() + ")");
    status("Verifying gtiup checksum and version...");
    if (sha256(candidate) != expected)
      throw Error("manager checksum mismatch");
    download(base + asset + ".licenses.txt", tmp.path / "licenses", 4 * 1024 * 1024, urls.test);
    if (trim(read(tmp.path / "licenses")).empty())
      throw Error("manager license notice is empty");
    fs::permissions(candidate, fs::perms::owner_all | fs::perms::group_read |
                                   fs::perms::group_exec | fs::perms::others_read |
                                   fs::perms::others_exec);
    if (manager_version(candidate) != "gtiup " + version)
      throw Error("downloaded manager version does not match release " + tag);
    const auto share = home / "share";
    const auto notices = share / "gtiup";
    const auto license = notices / "licenses.txt";
    if (fs::is_symlink(share) || fs::is_symlink(notices) || fs::is_symlink(license) ||
        (fs::exists(license) && !fs::is_regular_file(license)))
      throw Error("refusing to overwrite manager license path");
    fs::create_directories(notices);
    const bool had_license = fs::exists(license);
    // Keep recoverable originals on the same filesystem. The executable is
    // replaced with rename, so existing compiler/LSP invocations remain valid.
    fs::create_hard_link(destination, tmp.path / "old-gtiup");
    fs::copy_file(record, tmp.path / "old-manager.json");
    if (had_license)
      fs::copy_file(license, tmp.path / "old-licenses");
    atomic_write(tmp.path / "manager.json", Json{{"schema", 1}, {"version", version},
                                                {"sha256", expected}}.dump(2) + "\n");
    const auto inject_failure = [](const char *step) {
#ifdef GTIUP_TESTING
      if (env("GTIUP_TEST_UPDATE_FAILURE") == step)
        throw Error("injected manager update failure");
#else
      (void)step;
#endif
    };
    bool binary_changed = false, metadata_changed = false, license_changed = false;
    try {
      fs::rename(candidate, destination);
      binary_changed = true;
      inject_failure("binary");
      fs::rename(tmp.path / "manager.json", record);
      metadata_changed = true;
      inject_failure("metadata");
      fs::rename(tmp.path / "licenses", license);
      license_changed = true;
      inject_failure("licenses");
    } catch (...) {
      std::error_code failure, error;
      if (license_changed) {
        if (had_license)
          fs::rename(tmp.path / "old-licenses", license, error);
        else
          fs::remove(license, error);
        if (error) failure = error;
      }
      if (metadata_changed) {
        fs::rename(tmp.path / "old-manager.json", record, error);
        if (error) failure = error;
      }
      if (binary_changed) {
        fs::rename(tmp.path / "old-gtiup", destination, error);
        if (error) failure = error;
      }
      if (failure) {
        const auto recovery = tmp.path.string();
        tmp.path.clear(); // Retain originals for recovery rather than deleting them.
        throw Error("self update failed and rollback was incomplete: " + failure.message() +
                    "; recovery files: " + recovery);
      }
      throw;
    }
    std::cout << "Updated gtiup " << current << " -> " << version << '\n';
  }
  void bootstrap() {
    Lock lock(home);
    auto bin = home / "bin";
    fs::create_directories(bin);
    auto destination = bin / "gtiup";
    auto self = executable();
    for (const auto &name : {"gti", "gti_lsp"}) {
      auto p = bin / name;
      if ((fs::exists(p) || fs::is_symlink(p)) &&
          (!fs::is_symlink(p) || fs::read_symlink(p) != "gtiup"))
        throw Error("refusing to overwrite " + p.string());
    }
    if (fs::exists(destination) || fs::is_symlink(destination)) {
      if (fs::is_symlink(destination))
        throw Error("refusing to overwrite gtiup symlink");
      if (fs::canonical(destination) != self) {
        auto metadata = Json::parse(read(home / "manager.json"));
        if (metadata.value("schema", 0) != 1 ||
            metadata.at("sha256") != sha256(destination))
          throw Error(
              "existing gtiup is not the recorded manager; refusing overwrite");
      }
    }
    if (!fs::exists(destination) || fs::canonical(destination) != self) {
      Temp tmp(bin);
      fs::copy_file(self, tmp.path / "gtiup");
      fs::permissions(tmp.path / "gtiup",
                      fs::perms::owner_all | fs::perms::group_read |
                          fs::perms::group_exec | fs::perms::others_read |
                          fs::perms::others_exec);
      fs::rename(tmp.path / "gtiup", destination);
    }
    atomic_write(home / "manager.json", Json{{"schema", 1},
                                             {"version", GTIUP_VERSION},
                                             {"sha256", sha256(destination)}}
                                                .dump(2) +
                                            "\n");
    for (const auto &name : {"gti", "gti_lsp"})
      if (!fs::is_symlink(bin / name))
        fs::create_symlink("gtiup", bin / name);
    std::cout << "Installed gtiup " GTIUP_VERSION "\nAdd " << bin
              << " to PATH. Select a compiler with: gtiup default nightly\n";
  }
};
[[noreturn]] void execute(const fs::path &binary,
                          const std::vector<std::string> &args) {
  std::vector<std::string> words{binary.string()};
  words.insert(words.end(), args.begin(), args.end());
  std::vector<char *> argv;
  for (auto &s : words)
    argv.push_back(s.data());
  argv.push_back(nullptr);
  execv(binary.c_str(), argv.data());
  throw Error("cannot execute " + binary.string() + ": " +
              std::strerror(errno));
}
void help() {
  std::cout << "gtiup " GTIUP_VERSION " — GTI toolchain manager\n"
            << "  self-install [--default-toolchain CHANNEL|none]\n"
            << "  self update\n"
            << "  install|update|uninstall CHANNEL\n"
            << "  default CHANNEL|NAME\n"
            << "  link NAME PREFIX | unlink NAME\n"
            << "  update lsp [CHANNEL]\n"
            << "  list | show | which gti|gti_lsp | gc\n"
            << "  run CHANNEL|NAME gti|gti_lsp [ARGS...]\n"
            << "Channels: nightly, beta, stable, X.Y.Z, X.Y.Z-beta.N "
               "(preview is retired)\n"
            << "Linked toolchains: an existing install prefix registered with "
               "gtiup link; never updated or deleted by gtiup\n"
            << "Project selection: nearest gti-toolchain or gti-toolchain.toml; override: "
               "GTIUP_TOOLCHAIN or gti +CHANNEL\n";
}
int main(int argc, char **argv) {
  try {
    if (curl_global_init(CURL_GLOBAL_DEFAULT))
      throw Error("cannot initialize HTTPS");
    struct Cleanup {
      ~Cleanup() { curl_global_cleanup(); }
    } cleanup;
    Manager manager;
    std::vector<std::string> args(argv + 1, argv + argc);
    auto invoked = fs::path(argv[0]).filename().string();
    if (invoked == "gti" || invoked == "gti_lsp") {
      std::string channel;
      if (!args.empty() && args[0].size() && args[0][0] == '+') {
        channel = selection(args[0].substr(1));
        args.erase(args.begin());
      }
      execute(manager.which(invoked, channel), args);
    }
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
      help();
      return 0;
    }
    if (args[0] == "--version") {
      std::cout << "gtiup " GTIUP_VERSION "\n";
      return 0;
    }
    auto command = args[0];
    if (command == "self") {
      if (args.size() != 2 || args[1] != "update")
        throw Error("usage: gtiup self update");
      manager.self_update();
    } else if (command == "self-install") {
      if (args.size() != 1 &&
          (args.size() != 3 || args[1] != "--default-toolchain"))
        throw Error(
            "usage: gtiup self-install [--default-toolchain CHANNEL|none]");
      if (args.size() == 3 && args[2] != "none")
        installable(args[2]);
      manager.bootstrap();
      if (args.size() == 3 && args[2] != "none")
        manager.install(args[2], true);
    } else if (command == "update" && args.size() >= 2 && args[1] == "lsp") {
      if (args.size() > 3)
        throw Error("usage: gtiup update lsp [CHANNEL]");
      manager.update_lsp(args.size() == 3 ? args[2] : "");
    } else if (command == "install" || command == "update" ||
               command == "default" || command == "uninstall") {
      if (args.size() != 2)
        throw Error("usage: gtiup " + command + " CHANNEL");
      auto channel = selection(args[1]);
      if (command == "install" || command == "update")
        manager.install(channel);
      else if (command == "default") {
        if (!manager.state()["toolchains"].contains(channel)) {
          if (linked_name(channel))
            throw Error("no linked toolchain named " + channel +
                        "; register one with gtiup link " + channel + " PREFIX");
          manager.install(channel, true);
        }
        else {
          Lock lock(manager.home);
          auto s = manager.state();
          if (!s["toolchains"].contains(channel))
            throw Error("toolchain removed concurrently; retry");
          s["default"] = channel;
          manager.save(s);
        }
      } else {
        Lock lock(manager.home);
        auto s = manager.state();
        if (s["toolchains"].contains(channel) && s["toolchains"][channel].contains("linked"))
          throw Error(channel + " is a linked toolchain; remove it with gtiup unlink " + channel);
        if (s["default"] == channel)
          throw Error("select another default before uninstalling");
        if (!s["toolchains"].erase(channel))
          throw Error("toolchain is not installed");
        manager.save(s);
      }
    } else if (command == "link") {
      if (args.size() != 3)
        throw Error("usage: gtiup link NAME PREFIX");
      manager.link(args[1], args[2]);
    } else if (command == "unlink") {
      if (args.size() != 2)
        throw Error("usage: gtiup unlink NAME");
      manager.unlink(args[1]);
    } else if (command == "which") {
      if (args.size() != 2)
        throw Error("usage: gtiup which gti|gti_lsp");
      std::cout << manager.which(args[1]).string() << '\n';
    } else if (command == "run") {
      if (args.size() < 3)
        throw Error("usage: gtiup run CHANNEL TOOL [ARGS...]");
      execute(manager.which(args[2], args[1]), {args.begin() + 3, args.end()});
    } else if (command == "list" && args.size() == 1) {
      auto state = manager.state();
      for (const auto &[key, value] : state["toolchains"].items())
        if (value.contains("linked"))
          std::cout << key << " linked " << value.at("linked").get<std::string>() << '\n';
        else
          std::cout << key << ' ' << value.at("tag").get<std::string>() << ' '
                    << value.at("identity").get<std::string>() << '\n';
    } else if (command == "show" && args.size() == 1) {
      std::cout << manager.selected() << '\n'
                << manager.which("gti").string() << '\n';
      const auto record = manager.state()["toolchains"][manager.selected()];
      if (record.contains("lsp"))
        std::cout << "LSP component " << record["lsp"].at("source").get<std::string>() << '\n';
    } else if (command == "gc" && args.size() == 1) {
      Lock lock(manager.home);
      std::set<std::string> active;
      auto state = manager.state();
      // Linked toolchains live outside the store and are never collected.
      for (const auto &entry : state["toolchains"])
        if (entry.contains("directory"))
          active.insert(entry.at("directory").get<std::string>());
      auto store = manager.home / "toolchains";
      const std::regex managed(
          R"(^(preview|nightly|v[0-9]+\.[0-9]+\.[0-9]+(-beta\.[0-9]+)?)-[0-9a-f]{64}$)");
      if (fs::exists(store))
        for (const auto &p : fs::directory_iterator(store))
          if (!p.is_symlink() && p.is_directory() &&
              !active.count(p.path().filename().string()) &&
              std::regex_match(p.path().filename().string(), managed)) {
            fs::remove_all(p.path());
            std::cout << "Removed " << p.path().filename().string() << '\n';
          }
    } else
      throw Error("unknown command or invalid arguments; use gtiup --help");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "gtiup: " << e.what() << '\n';
    return 1;
  }
}
