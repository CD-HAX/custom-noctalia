#include "launcher/files_provider.h"

#include "config/config_service.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "launcher/fuzzy_match.h"
#include "shell/wayland/clipboard_service.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

  constexpr Logger kLog("files_provider");

  constexpr std::size_t kMaxResults = 50;
  constexpr std::size_t kMaxDetailContentBytes = 4096;
  constexpr auto kIndexCacheDuration = std::chrono::seconds(15);
  constexpr int kMaxTraverseDepth = 3;

  constexpr std::array<std::string_view, 10> kImageExtensions = {".png", ".jpg", ".jpeg", ".webp", ".svg",
                                                                 ".gif", ".bmp", ".ico",  ".avif", ".jxl"};

  constexpr std::array<std::string_view, 16> kTextExtensions = {".txt",  ".md",   ".markdown", ".toml",
                                                                ".json", ".yaml", ".yml",      ".xml",
                                                                ".csv",  ".log",  ".conf",     ".ini",
                                                                ".sh",   ".bash", ".zsh",      ".desktop"};

  constexpr std::array<std::string_view, 20> kCodeExtensions = {".c",    ".h",    ".cpp", ".hpp", ".cc",
                                                                ".cxx",  ".rs",   ".py",  ".go",  ".java",
                                                                ".js",   ".ts",   ".tsx", ".jsx", ".lua",
                                                                ".luau", ".html", ".qml", ".css", ".sql"};

  bool hasExtensionCaseInsensitive(std::string_view filename, std::string_view ext) {
    if (filename.size() < ext.size()) {
      return false;
    }
    const std::string_view suffix = filename.substr(filename.size() - ext.size());
    return std::ranges::equal(suffix, ext, [](char a, char b) {
      return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
  }

  bool isImageFile(std::string_view filename) {
    return std::ranges::any_of(kImageExtensions, [&](std::string_view ext) {
      return hasExtensionCaseInsensitive(filename, ext);
    });
  }

  bool isTextFile(std::string_view filename) {
    return std::ranges::any_of(kTextExtensions, [&](std::string_view ext) {
      return hasExtensionCaseInsensitive(filename, ext);
    });
  }

  bool isCodeFile(std::string_view filename) {
    return std::ranges::any_of(kCodeExtensions, [&](std::string_view ext) {
      return hasExtensionCaseInsensitive(filename, ext);
    });
  }

  std::string formatSize(std::uintmax_t bytes) {
    if (bytes == 0) {
      return "0 B";
    }
    constexpr std::array<const char*, 5> units = {"B", "KB", "MB", "GB", "TB"};
    auto b = static_cast<double>(bytes);
    std::size_t unitIdx = 0;
    while (b >= 1024.0 && unitIdx + 1 < units.size()) {
      b /= 1024.0;
      ++unitIdx;
    }
    std::ostringstream ss;
    if (unitIdx == 0) {
      ss << static_cast<std::uintmax_t>(b) << " " << units[unitIdx];
    } else {
      ss << std::fixed << std::setprecision(1) << b << " " << units[unitIdx];
    }
    return ss.str();
  }

  std::string formatDateTime(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tmVal{};
    if (::localtime_r(&t, &tmVal) == nullptr) {
      return {};
    }
    std::ostringstream ss;
    ss << std::put_time(&tmVal, "%Y-%m-%d %H:%M");
    return ss.str();
  }

  std::string readTextSnippet(const std::filesystem::path& path, std::size_t maxBytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      return {};
    }
    std::string buffer;
    buffer.resize(maxBytes);
    file.read(buffer.data(), static_cast<std::streamsize>(maxBytes));
    const auto bytesRead = file.gcount();
    buffer.resize(static_cast<std::size_t>(bytesRead));
    // Check if mostly printable ASCII/UTF-8
    const auto nonPrintable = std::ranges::count_if(buffer, [](char c) {
      const auto uc = static_cast<unsigned char>(c);
      return uc < 0x09 || (uc > 0x0D && uc < 0x20);
    });
    if (bytesRead > 0 && static_cast<std::size_t>(nonPrintable) * 10 > static_cast<std::size_t>(bytesRead)) {
      return "(Binary file content)";
    }
    return buffer;
  }

  void launchFile(const std::filesystem::path& path) {
    const pid_t pid = ::fork();
    if (pid == 0) {
      // Child process
      ::setsid();
      const std::string pathStr = path.string();
      const char* args[] = {"xdg-open", pathStr.c_str(), nullptr};
      ::execvp("xdg-open", const_cast<char* const*>(args));
      ::_exit(1);
    }
  }

  bool shouldSkipDirectory(const std::filesystem::path& p) {
    const std::string fn = p.filename().string();
    if (fn.empty()) {
      return false;
    }
    if (fn[0] == '.' && fn != "." && fn != "..") {
      return true;
    }
    return fn == "node_modules"
        || fn == "target"
        || fn == "build"
        || fn == "build-debug"
        || fn == "build-release"
        || fn == "__pycache__"
        || fn == ".git"
        || fn == "Trash";
  }

} // namespace

FilesProvider::FilesProvider(ConfigService* config, ClipboardService* clipboard)
    : m_config(config), m_clipboard(clipboard) {}

FilesProvider::~FilesProvider() = default;

std::string FilesProvider::displayName() const {
  const std::string tr = i18n::tr("launcher.providers.files.title");
  if (tr.starts_with("!!") && tr.ends_with("!!")) {
    return "Files";
  }
  return tr;
}

bool FilesProvider::goUp() {
  std::lock_guard lock(m_mutex);
  if (m_currentDirectory.empty()) {
    return false;
  }
  const char* home = std::getenv("HOME");
  if (home != nullptr && (m_currentDirectory == home || m_currentDirectory == "/")) {
    m_currentDirectory.clear();
    return true;
  }
  const auto parent = m_currentDirectory.parent_path();
  if (parent.empty() || parent == m_currentDirectory || (home != nullptr && m_currentDirectory == home)) {
    m_currentDirectory.clear();
  } else {
    m_currentDirectory = parent;
  }
  return true;
}

bool FilesProvider::canGoUp() const {
  std::lock_guard lock(m_mutex);
  return !m_currentDirectory.empty();
}

void FilesProvider::setCurrentDirectory(const std::filesystem::path& dir) {
  std::lock_guard lock(m_mutex);
  m_currentDirectory = dir;
}

const std::filesystem::path& FilesProvider::currentDirectory() const { return m_currentDirectory; }

std::string FilesProvider::currentDirectoryDisplay() const {
  std::lock_guard lock(m_mutex);
  if (m_currentDirectory.empty()) {
    return "Recent Folders";
  }
  const char* home = std::getenv("HOME");
  if (home != nullptr) {
    const std::string homeStr(home);
    const std::string pathStr = m_currentDirectory.string();
    if (pathStr == homeStr) {
      return "~";
    }
    if (pathStr.starts_with(homeStr)) {
      return "~" + pathStr.substr(homeStr.size());
    }
  }
  return m_currentDirectory.string();
}

std::vector<LauncherCategory> FilesProvider::categories() const {
  return {
      {.label = "Documents", .glyphName = "file-text"}, {.label = "Images", .glyphName = "photo"},
      {.label = "Code", .glyphName = "code"},           {.label = "Folders", .glyphName = "folder"},
      {.label = "Recent", .glyphName = "clock"},
  };
}

void FilesProvider::initialize() {
  std::lock_guard lock(m_mutex);
  m_indexedFiles.clear();
}

void FilesProvider::onLauncherOpened() {}

void FilesProvider::ensureIndexRefreshed() const {}

void FilesProvider::refreshIndex() const {
  std::lock_guard lock(m_mutex);
  m_indexedFiles.clear();
  m_lastIndexTime = std::chrono::steady_clock::now();
}

std::vector<LauncherResult> FilesProvider::query(std::string_view text) const {
  const std::string_view trimmed = StringUtils::trimLeftView(StringUtils::trimRightView(text));
  if (trimmed.empty()) {
    return queryBrowse();
  }
  if (trimmed.starts_with('/') || trimmed.starts_with('~') || trimmed.starts_with("./")) {
    return queryPath(trimmed);
  }
  return queryGlobal(trimmed);
}

std::vector<LauncherResult> FilesProvider::queryPrefixed(std::string_view text) const { return query(text); }

std::vector<LauncherResult> FilesProvider::queryBrowse() const {
  std::vector<LauncherResult> results;
  std::filesystem::path dirToBrowse;
  {
    std::lock_guard lock(m_mutex);
    dirToBrowse = m_currentDirectory;
  }

  std::error_code ec;
  const char* homeEnv = std::getenv("HOME");
  const std::filesystem::path home =
      (homeEnv != nullptr && homeEnv[0] != '\0') ? std::filesystem::path(homeEnv) : std::filesystem::current_path();

  if (dirToBrowse.empty()) {
    // Mode A: Top-level user folders in $HOME
    std::vector<std::filesystem::path> topDirs;
    for (const auto& entry :
         std::filesystem::directory_iterator(home, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (entry.is_directory(ec)) {
        const std::string fn = entry.path().filename().string();
        if (!fn.empty() && fn[0] != '.' && !shouldSkipDirectory(entry.path())) {
          topDirs.push_back(entry.path());
        }
      }
    }
    std::ranges::sort(topDirs, [](const auto& a, const auto& b) {
      return a.filename().string() < b.filename().string();
    });

    for (const auto& d : topDirs) {
      LauncherResult r;
      r.id = d.string();
      r.providerId = "files";
      r.title = d.filename().string();
      r.subtitle = "Folder • ~/" + d.filename().string();
      r.origin = "Files";
      r.originGlyph = "folder";
      r.glyphName = "folder";
      r.category = "Folders";
      r.score = 500.0;
      results.push_back(std::move(r));
    }
    return results;
  }

  // Mode B: Inside a specific directory
  if (!std::filesystem::is_directory(dirToBrowse, ec)) {
    return results;
  }

  // Prepend: .. (Parent Directory)
  LauncherResult parentItem;
  parentItem.id = "..";
  parentItem.providerId = "files";
  parentItem.title = ".. (Parent Directory)";
  parentItem.subtitle = "Parent Directory";
  parentItem.origin = "Files";
  parentItem.originGlyph = "arrow-up";
  parentItem.glyphName = "arrow-up";
  parentItem.category = "Folders";
  parentItem.score = 10000.0;
  results.push_back(std::move(parentItem));

  std::vector<CachedFileEntry> subdirs;
  std::vector<CachedFileEntry> files;

  for (const auto& entry : std::filesystem::directory_iterator(
           dirToBrowse, std::filesystem::directory_options::skip_permission_denied, ec
       )) {
    const std::string fn = entry.path().filename().string();
    if (fn.empty() || fn == "." || fn == "..") {
      continue;
    }

    CachedFileEntry cfe;
    cfe.path = entry.path();
    cfe.name = fn;
    cfe.lowerName = StringUtils::toLower(fn);
    cfe.isDirectory = entry.is_directory(ec);
    cfe.parentDisplay = dirToBrowse.string();

    if (!cfe.isDirectory) {
      cfe.size = entry.file_size(ec);
      if (ec) {
        cfe.size = 0;
        ec.clear();
      }
      cfe.isImage = isImageFile(fn);
      cfe.isText = isTextFile(fn);
      cfe.isCode = isCodeFile(fn);
    }
    const auto ftime = entry.last_write_time(ec);
    if (!ec) {
      cfe.modifiedTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
      );
    }

    if (cfe.isDirectory) {
      subdirs.push_back(std::move(cfe));
    } else {
      files.push_back(std::move(cfe));
    }
  }

  std::ranges::sort(subdirs, [](const auto& a, const auto& b) { return a.lowerName < b.lowerName; });
  std::ranges::sort(files, [](const auto& a, const auto& b) { return a.lowerName < b.lowerName; });

  for (const auto& s : subdirs) {
    results.push_back(createResult(s, 1000.0, false));
  }
  for (const auto& f : files) {
    results.push_back(createResult(f, 900.0, false));
  }

  return results;
}

std::vector<LauncherResult> FilesProvider::queryGlobal(std::string_view searchText) const {
  std::vector<LauncherResult> results;
  const std::string queryStr(searchText);
  const char* home = std::getenv("HOME");
  const std::string homeStr = home ? home : "";

  static const bool hasFd = (::access("/usr/bin/fd", X_OK) == 0);
  if (hasFd) {
    std::string cmd = "/usr/bin/fd --hidden --type f --type d --max-results 60 --max-depth 6 -F -i"
                      " --exclude .git --exclude .cache --exclude node_modules --exclude .local/share/Trash ";
    cmd += StringUtils::shellQuote(queryStr);
    if (!homeStr.empty()) {
      cmd += " " + StringUtils::shellQuote(homeStr);
    }
    for (const char* extraRoot : {"/etc", "/usr/share"}) {
      std::error_code ec;
      if (std::filesystem::is_directory(extraRoot, ec)) {
        cmd += " ";
        cmd += extraRoot;
      }
    }
    cmd += " 2>/dev/null";

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe != nullptr) {
      std::array<char, 4096> lineBuf{};
      std::error_code ec;
      while (::fgets(lineBuf.data(), static_cast<int>(lineBuf.size()), pipe) != nullptr) {
        std::string line(lineBuf.data());
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
          line.pop_back();
        }
        if (line.empty()) {
          continue;
        }
        const std::filesystem::path p(line);
        const bool isDir = std::filesystem::is_directory(p, ec);
        std::string fn = p.filename().string();
        if (fn.empty()) {
          fn = line;
        }

        CachedFileEntry cfe;
        cfe.path = p;
        cfe.name = fn;
        cfe.lowerName = StringUtils::toLower(fn);
        cfe.isDirectory = isDir;

        std::string parentDisplay = p.parent_path().string();
        if (!homeStr.empty() && parentDisplay.starts_with(homeStr)) {
          parentDisplay = "~" + parentDisplay.substr(homeStr.size());
        }
        cfe.parentDisplay = std::move(parentDisplay);

        if (!cfe.isDirectory) {
          cfe.size = std::filesystem::file_size(p, ec);
          if (ec) {
            cfe.size = 0;
            ec.clear();
          }
          cfe.isImage = isImageFile(fn);
          cfe.isText = isTextFile(fn);
          cfe.isCode = isCodeFile(fn);
        }
        const auto ftime = std::filesystem::last_write_time(p, ec);
        if (!ec) {
          cfe.modifiedTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
              ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
          );
        }

        double score = 100.0;
        const std::string lowerQ = StringUtils::toLower(queryStr);
        if (cfe.lowerName == lowerQ) {
          score = 1000.0;
        } else if (cfe.lowerName.starts_with(lowerQ)) {
          score = 500.0;
        } else if (cfe.lowerName.contains(lowerQ)) {
          score = 250.0;
        }
        results.push_back(createResult(cfe, score, false));
      }
      ::pclose(pipe);
      if (!results.empty()) {
        std::ranges::sort(results, [](const auto& a, const auto& b) { return a.score > b.score; });
        return results;
      }
    }
  }

  return queryFuzzy(searchText);
}

std::vector<LauncherResult> FilesProvider::queryPath(std::string_view pathText) const {
  std::vector<LauncherResult> results;
  const std::filesystem::path rawPath = FileUtils::expandUserPath(std::string(pathText));

  std::error_code ec;
  std::filesystem::path dirPath;
  std::string prefixFilter;

  if (std::filesystem::is_directory(rawPath, ec)) {
    dirPath = rawPath;
  } else {
    dirPath = rawPath.parent_path();
    prefixFilter = rawPath.filename().string();
  }

  if (dirPath.empty() || !std::filesystem::is_directory(dirPath, ec)) {
    return results;
  }

  std::filesystem::directory_iterator it(dirPath, std::filesystem::directory_options::skip_permission_denied, ec);
  std::filesystem::directory_iterator end;

  const std::string lowerPrefix = StringUtils::toLower(prefixFilter);
  while (!ec && it != end && results.size() < kMaxResults) {
    const auto& entry = *it;
    const std::string fn = entry.path().filename().string();
    const std::string lowerFn = StringUtils::toLower(fn);

    if (lowerPrefix.empty() || lowerFn.starts_with(lowerPrefix) || lowerFn.contains(lowerPrefix)) {
      CachedFileEntry cfe;
      cfe.path = entry.path();
      cfe.name = fn;
      cfe.lowerName = lowerFn;
      cfe.isDirectory = entry.is_directory(ec);
      cfe.parentDisplay = dirPath.string();
      if (!cfe.isDirectory) {
        cfe.size = entry.file_size(ec);
        cfe.isImage = isImageFile(fn);
        cfe.isText = isTextFile(fn);
        cfe.isCode = isCodeFile(fn);
      }
      const auto ftime = entry.last_write_time(ec);
      if (!ec) {
        cfe.modifiedTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
        );
      }
      results.push_back(createResult(cfe, 1000.0, false));
    }
    it.increment(ec);
  }

  return results;
}

std::vector<LauncherResult> FilesProvider::queryFuzzy(std::string_view searchText) const {
  ensureIndexRefreshed();
  std::lock_guard lock(m_mutex);

  std::vector<LauncherResult> results;
  if (searchText.empty()) {
    // Show top recent files
    for (std::size_t i = 0; i < std::min(m_indexedFiles.size(), kMaxResults); ++i) {
      results.push_back(createResult(m_indexedFiles[i], 100.0, false));
    }
    return results;
  }

  const std::string lowerPattern = StringUtils::toLower(searchText);

  struct ScoredEntry {
    const CachedFileEntry* entry = nullptr;
    double score = 0.0;
  };

  std::vector<ScoredEntry> scored;
  scored.reserve(m_indexedFiles.size());

  for (const auto& file : m_indexedFiles) {
    double score = FuzzyMatch::score(lowerPattern, file.lowerName);
    if (!FuzzyMatch::isMatch(score)) {
      if (file.lowerName.contains(lowerPattern)) {
        score = 50.0;
      } else {
        continue;
      }
    }

    if (file.lowerName.starts_with(lowerPattern)) {
      score += 150.0;
    }
    scored.push_back({.entry = &file, .score = score});
  }

  std::ranges::sort(scored, [](const ScoredEntry& a, const ScoredEntry& b) { return a.score > b.score; });

  const bool singleResultDetail = (scored.size() == 1);
  for (std::size_t i = 0; i < std::min(scored.size(), kMaxResults); ++i) {
    results.push_back(createResult(*scored[i].entry, scored[i].score, singleResultDetail));
  }

  return results;
}

LauncherResult FilesProvider::createResult(const CachedFileEntry& entry, double score, bool detail) const {
  LauncherResult r;
  r.id = entry.path.string();
  r.providerId = "files";
  r.title = entry.name;
  r.origin = "Files";
  r.originGlyph = entry.isDirectory ? "folder" : "file";
  r.score = score;

  const std::string modStr = formatDateTime(entry.modifiedTime);

  if (entry.isDirectory) {
    r.category = "Folders";
    r.glyphName = "folder";
    r.subtitle = "Folder • " + modStr + " • " + entry.parentDisplay;
  } else {
    const std::string sizeStr = formatSize(entry.size);
    if (entry.isImage) {
      r.category = "Images";
      r.glyphName = "photo";
      r.iconPath = entry.path.string(); // Renders live image thumbnail!
      r.subtitle = sizeStr + " • " + modStr + " • " + entry.parentDisplay;
    } else if (entry.isCode) {
      r.category = "Code";
      r.glyphName = "code";
      r.subtitle = sizeStr + " • " + modStr + " • " + entry.parentDisplay;
    } else {
      r.category = "Documents";
      r.glyphName = entry.isText ? "file-text" : "file";
      r.subtitle = sizeStr + " • " + modStr + " • " + entry.parentDisplay;
    }

    if (entry.isText || entry.isCode) {
      const std::string snippet = readTextSnippet(entry.path, 120);
      if (!snippet.empty() && snippet != "(Binary file content)") {
        std::string singleLine = StringUtils::windowTitleSingleLine(snippet);
        if (singleLine.size() > 60) {
          singleLine = singleLine.substr(0, 57) + "...";
        }
        r.subtitle = singleLine + " (" + sizeStr + ")";
      }
    }
  }

  if (detail) {
    r.presentation = "detail";
    if (entry.isDirectory) {
      std::string folderContents;
      std::error_code ec;
      std::size_t count = 0;
      for (const auto& child : std::filesystem::directory_iterator(
               entry.path, std::filesystem::directory_options::skip_permission_denied, ec
           )) {
        if (count < 25) {
          if (!folderContents.empty()) {
            folderContents += "\n";
          }
          folderContents += (child.is_directory(ec) ? "📁 " : "📄 ") + child.path().filename().string();
        }
        count++;
      }
      r.title = folderContents.empty() ? "(Empty folder)" : folderContents;
      r.subtitle = entry.path.string() + " • " + std::to_string(count) + " items • " + modStr;
    } else if (entry.isText || entry.isCode) {
      const std::string content = readTextSnippet(entry.path, kMaxDetailContentBytes);
      r.title = content.empty() ? entry.name : content;
      r.subtitle = entry.path.string() + " • " + formatSize(entry.size) + " • " + modStr;
    } else {
      r.subtitle = entry.path.string() + " • " + formatSize(entry.size) + " • " + modStr;
    }
  }

  return r;
}

bool FilesProvider::activate(const LauncherResult& result) {
  if (result.id.empty()) {
    return false;
  }
  if (result.id == "..") {
    goUp();
    return false;
  }
  std::error_code ec;
  if (std::filesystem::is_directory(result.id, ec)) {
    setCurrentDirectory(result.id);
    return false;
  }
  launchFile(std::filesystem::path(result.id));
  return true;
}

FilesProvider::FileMetadata FilesProvider::getFileMetadata(std::string_view pathStr) {
  FileMetadata meta;
  if (pathStr == "..") {
    meta.isParentNav = true;
    meta.name = ".. (Parent Directory)";
    meta.type = "inode/directory";
    meta.path = "..";
    return meta;
  }

  std::error_code ec;
  const std::filesystem::path path(pathStr);
  const auto status = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::exists(status)) {
    meta.name = path.filename().string();
    meta.path = path.string();
    meta.type = "unknown";
    meta.size = "0 B";
    return meta;
  }

  meta.name = path.filename().string();
  meta.path = path.string();
  meta.isDirectory = std::filesystem::is_directory(status);

  if (meta.isDirectory) {
    meta.type = "inode/directory";
    meta.size = formatSize(std::filesystem::file_size(path, ec));
    if (ec) {
      meta.size = "4.0 KB";
      ec.clear();
    }
    std::size_t items = 0;
    for (const auto& _ :
         std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec)) {
      items++;
      if (items >= 500) {
        break;
      }
    }
    meta.items = (items >= 500) ? "500+" : std::to_string(items);
  } else {
    const std::uintmax_t sz = std::filesystem::file_size(path, ec);
    meta.size = ec ? "0 B" : formatSize(sz);
    ec.clear();

    const std::string ext = StringUtils::toLower(path.extension().string());
    meta.isImage = isImageFile(meta.name);
    meta.isText = isTextFile(meta.name) || isCodeFile(meta.name);

    if (ext == ".png") {
      meta.type = "image/png";
    } else if (ext == ".jpg" || ext == ".jpeg") {
      meta.type = "image/jpeg";
    } else if (ext == ".webp") {
      meta.type = "image/webp";
    } else if (ext == ".svg") {
      meta.type = "image/svg+xml";
    } else if (ext == ".gif") {
      meta.type = "image/gif";
    } else if (ext == ".txt") {
      meta.type = "text/plain";
    } else if (ext == ".md") {
      meta.type = "text/markdown";
    } else if (ext == ".json") {
      meta.type = "application/json";
    } else if (ext == ".toml") {
      meta.type = "application/toml";
    } else if (ext == ".yaml" || ext == ".yml") {
      meta.type = "text/yaml";
    } else if (ext == ".xml") {
      meta.type = "text/xml";
    } else if (ext == ".sh") {
      meta.type = "application/x-sh";
    } else if (ext == ".py") {
      meta.type = "text/x-python";
    } else if (ext == ".cpp" || ext == ".cc") {
      meta.type = "text/x-c++src";
    } else if (ext == ".c") {
      meta.type = "text/x-csrc";
    } else if (ext == ".h" || ext == ".hpp") {
      meta.type = "text/x-chdr";
    } else if (ext == ".rs") {
      meta.type = "text/x-rust";
    } else if (ext == ".go") {
      meta.type = "text/x-go";
    } else if (ext == ".js") {
      meta.type = "application/javascript";
    } else if (ext == ".ts") {
      meta.type = "application/typescript";
    } else if (meta.isText) {
      meta.type = "text/plain";
    } else {
      meta.type = "application/octet-stream";
    }

    if (meta.isText) {
      std::ifstream f(path, std::ios::binary);
      if (f.is_open()) {
        std::string buf;
        buf.resize(4096);
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto readBytes = f.gcount();
        buf.resize(static_cast<std::size_t>(readBytes));
        meta.textContent = buf;

        std::size_t lines = 0;
        std::size_t chars = static_cast<std::size_t>(sz);
        if (sz <= 128 * 1024) {
          f.clear();
          f.seekg(0);
          std::string line;
          while (std::getline(f, line) && lines < 2000) {
            lines++;
          }
          meta.lines = (lines >= 2000) ? "2000+" : std::to_string(lines);
        } else {
          for (char ch : buf) {
            if (ch == '\n') {
              lines++;
            }
          }
          const std::size_t safeReadBytes = readBytes > 0 ? static_cast<std::size_t>(readBytes) : 1U;
          const std::size_t est = lines * (sz / safeReadBytes);
          meta.lines = std::to_string(std::max<std::size_t>(1, est));
        }
        meta.chars = std::to_string(chars);
      }
    }
  }

  return meta;
}

bool FilesProvider::openContainingFolder(std::string_view path) const {
  if (path.empty()) {
    return false;
  }
  const std::filesystem::path p(path);
  launchFile(p.parent_path());
  return true;
}

bool FilesProvider::copyPath(std::string_view path) const {
  if (m_clipboard == nullptr || path.empty()) {
    return false;
  }
  return m_clipboard->copyText(std::string(path));
}

bool FilesProvider::copyFileContents(std::string_view path) const {
  if (m_clipboard == nullptr || path.empty()) {
    return false;
  }
  const std::string content = readTextSnippet(std::filesystem::path(path), 65536);
  if (content.empty() || content == "(Binary file content)") {
    return false;
  }
  return m_clipboard->copyText(content);
}

std::vector<TooltipRow> FilesProvider::getTooltipRows(std::string_view pathStr) {
  std::vector<TooltipRow> rows;
  std::error_code ec;
  const std::filesystem::path path(pathStr);
  const auto status = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::exists(status)) {
    return rows;
  }

  const std::string filename = path.filename().string();
  const bool isDir = std::filesystem::is_directory(status);

  // 1. Name
  rows.push_back(TooltipRow{.key = "Name", .value = filename});

  // 2. Type
  if (isDir) {
    rows.push_back(TooltipRow{.key = "Type", .value = "Directory"});
  } else {
    std::string typeDesc = "File";
    const std::string ext = StringUtils::toLower(path.extension().string());
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".gif" || ext == ".svg") {
      typeDesc = "Image (" + (ext.empty() ? "" : ext.substr(1)) + ")";
    } else if (
        ext == ".cpp"
        || ext == ".c"
        || ext == ".h"
        || ext == ".hpp"
        || ext == ".rs"
        || ext == ".go"
        || ext == ".py"
        || ext == ".js"
        || ext == ".ts"
    ) {
      typeDesc = "Source Code (" + (ext.empty() ? "" : ext.substr(1)) + ")";
    } else if (
        ext == ".txt"
        || ext == ".md"
        || ext == ".json"
        || ext == ".toml"
        || ext == ".yaml"
        || ext == ".yml"
        || ext == ".sh"
    ) {
      typeDesc = "Text Document";
    } else if (ext == ".pdf") {
      typeDesc = "PDF Document";
    } else if (ext == ".zip" || ext == ".tar" || ext == ".gz" || ext == ".xz" || ext == ".zst") {
      typeDesc = "Archive";
    } else if (!ext.empty()) {
      typeDesc = ext.substr(1) + " file";
    }
    rows.push_back(TooltipRow{.key = "Type", .value = typeDesc});
  }

  // 3. For directories: Items count and Contents!
  if (isDir) {
    std::size_t itemCount = 0;
    std::vector<std::string> childNames;
    for (const auto& child :
         std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec)) {
      itemCount++;
      if (childNames.size() < 7) {
        std::string name = child.path().filename().string();
        if (child.is_directory(ec)) {
          name += "/";
        }
        childNames.push_back(std::move(name));
      }
    }
    rows.push_back(
        TooltipRow{.key = "Items", .value = std::to_string(itemCount) + (itemCount == 1 ? " item" : " items")}
    );
    if (!childNames.empty()) {
      std::string contentsSummary;
      for (std::size_t i = 0; i < childNames.size(); ++i) {
        if (i > 0) {
          contentsSummary += ", ";
        }
        contentsSummary += childNames[i];
      }
      if (itemCount > childNames.size()) {
        contentsSummary += ", +" + std::to_string(itemCount - childNames.size()) + " more";
      }
      rows.push_back(TooltipRow{.key = "Contents", .value = contentsSummary});
    }
  } else {
    // 3. For files: Size
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec) {
      rows.push_back(TooltipRow{.key = "Size", .value = formatSize(size)});
    }
  }

  // 4. Modified timestamp
  const auto ftime = std::filesystem::last_write_time(path, ec);
  if (!ec) {
    const auto sysTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
    );
    rows.push_back(TooltipRow{.key = "Modified", .value = formatDateTime(sysTime)});
  }

  // 5. Path (relative to home if inside home)
  std::string pathDisplay = path.string();
  if (const char* home = std::getenv("HOME")) {
    const std::string homeStr = home;
    if (pathDisplay.starts_with(homeStr)) {
      pathDisplay = "~" + pathDisplay.substr(homeStr.size());
    }
  }
  rows.push_back(TooltipRow{.key = "Path", .value = pathDisplay});

  // 6. For text files: Lines & preview snippet
  if (!isDir && (isTextFile(filename) || isCodeFile(filename))) {
    std::ifstream stream(path);
    if (stream) {
      std::string firstLine;
      std::size_t lineCount = 0;
      std::string line;
      while (std::getline(stream, line)) {
        lineCount++;
        if (firstLine.empty() && !line.empty()) {
          firstLine = line;
        }
      }
      rows.push_back(TooltipRow{.key = "Lines", .value = std::to_string(lineCount)});
      if (!firstLine.empty()) {
        firstLine = StringUtils::windowTitleSingleLine(firstLine);
        if (firstLine.size() > 50) {
          firstLine = firstLine.substr(0, 47) + "...";
        }
        rows.push_back(TooltipRow{.key = "Preview", .value = firstLine});
      }
    }
  }

  return rows;
}

std::string FilesProvider::getTooltipString(std::string_view pathStr) {
  auto rows = getTooltipRows(pathStr);
  if (rows.empty()) {
    return {};
  }
  std::string s;
  for (const auto& r : rows) {
    if (!s.empty()) {
      s += "\n";
    }
    s += r.key + ": " + r.value;
  }
  return s;
}
