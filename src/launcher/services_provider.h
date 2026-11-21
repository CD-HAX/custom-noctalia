#pragma once

#include "launcher/launcher_provider.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class ClipboardService;
class SessionBus;
class SystemBus;

class ServicesProvider final : public LauncherProvider {
public:
  ServicesProvider(SystemBus* systemBus, SessionBus* sessionBus, ClipboardService* clipboard);
  ~ServicesProvider() override;

  std::string_view defaultPrefix() const override { return "services"; }
  bool defaultIncludeInGlobalSearch() const override { return false; }
  std::string_view id() const override { return "services"; }
  std::string displayName() const override;
  std::string_view defaultGlyphName() const override { return "server"; }
  bool supportsGrid() const override { return false; }
  bool trackUsage() const override { return true; }

  std::vector<LauncherCategory> categories() const override;

  void initialize() override;
  void onLauncherOpened() override;

  std::vector<LauncherResult> query(std::string_view text) const override;
  std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

  // Actions callable from UI context menu or direct commands
  bool startService(std::string_view unitName, bool isUser) const;
  bool stopService(std::string_view unitName, bool isUser) const;
  bool restartService(std::string_view unitName, bool isUser) const;
  bool enableService(std::string_view unitName, bool isUser) const;
  bool disableService(std::string_view unitName, bool isUser) const;
  bool copyUnitName(std::string_view unitName) const;

private:
  struct ServiceInfo {
    std::string unitName;
    std::string cleanName;
    std::string description;
    std::string activeState;
    std::string subState;
    std::string unitFileState;
    bool isUser = false;
  };

  void ensureServicesRefreshed() const;
  void refreshServices() const;
  void scanSystemdUnitsFromDisk() const;
  bool executeAction(std::string_view unitName, bool isUser, std::string_view action) const;
  LauncherResult createServiceResult(const ServiceInfo& s, double score) const;
  LauncherResult createActionResult(std::string_view action, const ServiceInfo& s, double score) const;

  SystemBus* m_systemBus = nullptr;
  SessionBus* m_sessionBus = nullptr;
  ClipboardService* m_clipboard = nullptr;

  mutable std::mutex m_mutex;
  mutable std::vector<ServiceInfo> m_cachedServices;
  mutable std::chrono::steady_clock::time_point m_lastRefreshTime;
};
