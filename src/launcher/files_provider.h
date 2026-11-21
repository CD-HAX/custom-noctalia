#pragma once

#include "launcher/launcher_provider.h"
#include "render/scene/tooltip_content.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class ClipboardService;
class ConfigService;

class FilesProvider final : public LauncherProvider {
public:
  FilesProvider(ConfigService* config, ClipboardService* clipboard);
  ~FilesProvider() override;

  std::string_view defaultPrefix() const override { return "files"; }
  bool defaultIncludeInGlobalSearch() const override { return true; }
  std::string_view id() const override { return "files"; }
  std::string displayName() const override;
  std::string_view defaultGlyphName() const override { return "folder"; }
  bool supportsGrid() const override { return false; }
  bool trackUsage() const override { return true; }

  std::vector<LauncherCategory> categories() const override;

  void initialize() override;
  void onLauncherOpened() override;

  std::vector<LauncherResult> query(std::string_view text) const override;
  std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

  // Context menu actions for a file result
  bool openContainingFolder(std::string_view path) const;
  bool copyPath(std::string_view path) const;
  bool copyFileContents(std::string_view path) const;

  // Navigation for directory browsing
  bool goUp();
  bool canGoUp() const;
  void setCurrentDirectory(const std::filesystem::path& dir);
  const std::filesystem::path& currentDirectory() const;
  std::string currentDirectoryDisplay() const;

  // Rich metadata and folder contents inspection for inspector panel & hover tooltips
  struct FileMetadata {
    std::string name;
    std::string type;
    std::string size;
    std::string path;
    std::string items;
    std::string lines;
    std::string chars;
    std::string textContent;
    bool isDirectory = false;
    bool isImage = false;
    bool isText = false;
    bool isParentNav = false;
  };

  static FileMetadata getFileMetadata(std::string_view path);
  static std::vector<TooltipRow> getTooltipRows(std::string_view path);
  static std::string getTooltipString(std::string_view path);

private:
  struct CachedFileEntry {
    std::filesystem::path path;
    std::string name;
    std::string lowerName;
    std::string parentDisplay;
    std::uintmax_t size = 0;
    std::chrono::system_clock::time_point modifiedTime;
    bool isDirectory = false;
    bool isImage = false;
    bool isText = false;
    bool isCode = false;
  };

  void ensureIndexRefreshed() const;
  void refreshIndex() const;
  std::vector<LauncherResult> queryBrowse() const;
  std::vector<LauncherResult> queryGlobal(std::string_view searchText) const;
  std::vector<LauncherResult> queryPath(std::string_view pathText) const;
  std::vector<LauncherResult> queryFuzzy(std::string_view searchText) const;
  LauncherResult createResult(const CachedFileEntry& entry, double score, bool detail) const;

  ConfigService* m_config = nullptr;
  ClipboardService* m_clipboard = nullptr;

  mutable std::mutex m_mutex;
  mutable std::filesystem::path m_currentDirectory;
  mutable std::vector<CachedFileEntry> m_indexedFiles;
  mutable std::chrono::steady_clock::time_point m_lastIndexTime;
};
