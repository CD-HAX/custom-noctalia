#include "launcher/files_provider.h"
#include "tests/test_check.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
  const fs::path root = fs::temp_directory_path() / ("noctalia-files-provider-" + std::to_string(getpid()));
  fs::create_directories(root);

  // Create test files
  const fs::path textFile = root / "sample.txt";
  {
    std::ofstream ofs(textFile);
    ofs << "Hello Noctalia Files Provider!";
  }

  const fs::path imgFile = root / "preview.png";
  {
    std::ofstream ofs(imgFile);
    ofs << "fake image bytes";
  }

  const fs::path subDir = root / "subfolder";
  fs::create_directories(subDir);

  FilesProvider provider(nullptr, nullptr);
  provider.initialize();
  provider.onLauncherOpened();

  TEST_CHECK(provider.id() == "files");
  TEST_CHECK(provider.defaultPrefix() == "files");
  TEST_CHECK(provider.defaultIncludeInGlobalSearch());
  TEST_CHECK(!provider.categories().empty());

  // Test direct path query
  const auto directResults = provider.query(root.string());
  TEST_CHECK(!directResults.empty());

  const auto textResults = provider.query(textFile.string());
  TEST_CHECK(!textResults.empty());
  TEST_CHECK(textResults.front().title == "sample.txt");
  TEST_CHECK(textResults.front().subtitle.find("Hello Noctalia") != std::string::npos);

  // Test image result iconPath preview
  const auto imgResults = provider.query(imgFile.string());
  TEST_CHECK(!imgResults.empty());
  TEST_CHECK(imgResults.front().iconPath == imgFile.string());

  // Test prefixed query
  const auto prefResults = provider.queryPrefixed(root.string());
  TEST_CHECK(!prefResults.empty());

  // Test copy without clipboard returns false gracefully
  TEST_CHECK(!provider.copyPath(textFile.string()));
  TEST_CHECK(!provider.copyFileContents(textFile.string()));

  // Test tooltip metadata rows on file
  const auto fileTooltipRows = FilesProvider::getTooltipRows(textFile.string());
  TEST_CHECK(!fileTooltipRows.empty());
  bool foundName = false;
  bool foundType = false;
  for (const auto& row : fileTooltipRows) {
    if (row.key == "Name" && row.value == "sample.txt") {
      foundName = true;
    }
    if (row.key == "Type") {
      foundType = true;
    }
  }
  TEST_CHECK(foundName);
  TEST_CHECK(foundType);

  // Test tooltip metadata rows and folder contents on directory
  const auto dirTooltipRows = FilesProvider::getTooltipRows(root.string());
  TEST_CHECK(!dirTooltipRows.empty());
  bool foundItems = false;
  bool foundContents = false;
  for (const auto& row : dirTooltipRows) {
    if (row.key == "Items") {
      foundItems = true;
    }
    if (row.key == "Contents") {
      foundContents = true;
    }
  }
  TEST_CHECK(foundItems);
  TEST_CHECK(foundContents);

  // Test directory navigation methods
  TEST_CHECK(!provider.canGoUp());
  provider.setCurrentDirectory(subDir);
  TEST_CHECK(provider.canGoUp());
  TEST_CHECK(provider.currentDirectory() == subDir);
  TEST_CHECK(!provider.currentDirectoryDisplay().empty());

  // Test getFileMetadata
  const auto meta = FilesProvider::getFileMetadata(textFile.string());
  TEST_CHECK(meta.name == "sample.txt");
  TEST_CHECK(meta.isText);
  TEST_CHECK(!meta.isDirectory);
  TEST_CHECK(meta.type == "text/plain");

  const auto dirMeta = FilesProvider::getFileMetadata(root.string());
  TEST_CHECK(dirMeta.isDirectory);
  TEST_CHECK(dirMeta.type == "inode/directory");

  // In subDir, query("") should have ".." as first item
  const auto subDirResults = provider.query("");
  TEST_CHECK(!subDirResults.empty());
  TEST_CHECK(subDirResults.front().id == "..");

  // Activating ".." should goUp and return false (staying in launcher)
  TEST_CHECK(!provider.activate(subDirResults.front()));
  TEST_CHECK(provider.currentDirectory() == root);

  // Navigating up from root
  TEST_CHECK(provider.goUp());

  std::error_code ec;
  fs::remove_all(root, ec);
  return 0;
}
