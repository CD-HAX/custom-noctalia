#include "launcher/services_provider.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"
#include "launcher/fuzzy_match.h"
#include "shell/wayland/clipboard_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

  constexpr Logger kLog("services_provider");
  constexpr auto kCacheDuration = std::chrono::seconds(5);
  constexpr std::size_t kMaxResults = 50;

  const sdbus::ServiceName kSystemdName{"org.freedesktop.systemd1"};
  const sdbus::ObjectPath kSystemdPath{"/org/freedesktop/systemd1"};
  constexpr auto kSystemdManagerInterface = "org.freedesktop.systemd1.Manager";

  using UnitTuple = sdbus::Struct<
      std::string, std::string, std::string, std::string, std::string, std::string, sdbus::ObjectPath, std::uint32_t,
      std::string, sdbus::ObjectPath>;

  std::string cleanServiceName(std::string_view unitName) {
    if (unitName.ends_with(".service")) {
      return std::string(unitName.substr(0, unitName.size() - 8));
    }
    return std::string(unitName);
  }

  void runSystemctlCommand(const std::vector<std::string>& args) {
    const pid_t pid = ::fork();
    if (pid == 0) {
      ::setsid();
      std::vector<char*> cArgs;
      cArgs.reserve(args.size() + 1);
      for (const auto& a : args) {
        cArgs.push_back(const_cast<char*>(a.c_str()));
      }
      cArgs.push_back(nullptr);
      ::execvp(cArgs[0], cArgs.data());
      ::_exit(1);
    }
  }

} // namespace

ServicesProvider::ServicesProvider(SystemBus* systemBus, SessionBus* sessionBus, ClipboardService* clipboard)
    : m_systemBus(systemBus), m_sessionBus(sessionBus), m_clipboard(clipboard) {}

ServicesProvider::~ServicesProvider() = default;

std::string ServicesProvider::displayName() const {
  const std::string tr = i18n::tr("launcher.providers.services.title");
  if (tr.starts_with("!!") && tr.ends_with("!!")) {
    return "Services";
  }
  return tr;
}

std::vector<LauncherCategory> ServicesProvider::categories() const {
  return {
      {.label = "Active", .glyphName = "player-play"},
      {.label = "Inactive", .glyphName = "player-stop"},
      {.label = "Failed", .glyphName = "alert-triangle"},
      {.label = "System", .glyphName = "server"},
      {.label = "User", .glyphName = "user"},
  };
}

void ServicesProvider::initialize() { refreshServices(); }

void ServicesProvider::onLauncherOpened() { ensureServicesRefreshed(); }

void ServicesProvider::ensureServicesRefreshed() const {
  const auto now = std::chrono::steady_clock::now();
  if (now - m_lastRefreshTime > kCacheDuration) {
    refreshServices();
  }
}

void ServicesProvider::refreshServices() const {
  std::lock_guard lock(m_mutex);
  m_cachedServices.clear();
  m_lastRefreshTime = std::chrono::steady_clock::now();

  bool foundAny = false;

  auto queryDbus = [&](sdbus::IConnection& conn, bool isUser) {
    try {
      auto proxy = sdbus::createProxy(conn, kSystemdName, kSystemdPath);
      std::vector<UnitTuple> units;
      proxy->callMethod("ListUnits").onInterface(kSystemdManagerInterface).storeResultsTo(units);

      for (auto& u : units) {
        const std::string& name = u.get<0>();
        if (!name.ends_with(".service")) {
          continue;
        }

        ServiceInfo info;
        info.unitName = name;
        info.cleanName = cleanServiceName(name);
        info.description = u.get<1>();
        info.activeState = u.get<3>();
        info.subState = u.get<4>();
        info.isUser = isUser;

        // Try getting unit file state
        try {
          std::string fileState;
          proxy->callMethod("GetUnitFileState")
              .onInterface(kSystemdManagerInterface)
              .withArguments(name)
              .storeResultsTo(fileState);
          info.unitFileState = std::move(fileState);
        } catch (...) {
          info.unitFileState = "unknown";
        }

        m_cachedServices.push_back(std::move(info));
        foundAny = true;
      }
    } catch (const sdbus::Error& e) {
      kLog.debug("ListUnits failed (isUser={}): {}", isUser, e.what());
    }
  };

  if (m_systemBus != nullptr) {
    queryDbus(m_systemBus->connection(), false);
  }
  if (m_sessionBus != nullptr) {
    queryDbus(m_sessionBus->connection(), true);
  }

  if (!foundAny) {
    scanSystemdUnitsFromDisk();
  }
}

void ServicesProvider::scanSystemdUnitsFromDisk() const {
  const std::array<std::pair<const char*, bool>, 3> dirs = {{
      {"/usr/lib/systemd/system", false},
      {"/etc/systemd/system", false},
      {"/usr/lib/systemd/user", true},
  }};

  for (const auto& [dirPath, isUser] : dirs) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dirPath, ec)) {
      continue;
    }
    std::filesystem::directory_iterator it(dirPath, ec);
    std::filesystem::directory_iterator end;
    while (!ec && it != end) {
      const auto& entry = *it;
      const std::string fn = entry.path().filename().string();
      if (fn.ends_with(".service") && !fn.contains('@')) {
        // Read Description from unit file if possible
        std::string desc;
        std::ifstream file(entry.path());
        std::string line;
        while (std::getline(file, line)) {
          if (line.starts_with("Description=")) {
            desc = line.substr(12);
            break;
          }
        }

        ServiceInfo info;
        info.unitName = fn;
        info.cleanName = cleanServiceName(fn);
        info.description = desc.empty() ? fn : desc;
        info.activeState = "inactive";
        info.subState = "dead";
        info.unitFileState = "enabled";
        info.isUser = isUser;

        // Check if not already added
        if (std::ranges::none_of(m_cachedServices, [&](const ServiceInfo& s) {
              return s.unitName == info.unitName && s.isUser == info.isUser;
            })) {
          m_cachedServices.push_back(std::move(info));
        }
      }
      it.increment(ec);
    }
  }
}

std::vector<LauncherResult> ServicesProvider::query(std::string_view text) const {
  ensureServicesRefreshed();
  std::lock_guard lock(m_mutex);

  std::vector<LauncherResult> results;
  const std::string_view trimmed = StringUtils::trimLeftView(StringUtils::trimRightView(text));

  // Check for action prefixes like "start <service>", "stop <service>", etc.
  constexpr std::array<std::string_view, 5> actions = {"start", "stop", "restart", "enable", "disable"};
  for (std::string_view action : actions) {
    if (trimmed.starts_with(action) && (trimmed.size() == action.size() || trimmed[action.size()] == ' ')) {
      std::string_view target = trimmed.substr(action.size());
      target = StringUtils::trimLeftView(target);
      const std::string lowerTarget = StringUtils::toLower(target);

      for (const auto& s : m_cachedServices) {
        if (lowerTarget.empty() || s.cleanName.contains(lowerTarget) || s.unitName.contains(lowerTarget)) {
          results.push_back(createActionResult(action, s, 1000.0));
          if (results.size() >= kMaxResults) {
            break;
          }
        }
      }
      if (results.empty() && !lowerTarget.empty()) {
        ServiceInfo customService;
        customService.unitName = lowerTarget.ends_with(".service") ? lowerTarget : lowerTarget + ".service";
        customService.cleanName = lowerTarget;
        customService.description = "System service";
        customService.isUser = false;
        results.push_back(createActionResult(action, customService, 1000.0));
      }
      return results;
    }
  }

  if (trimmed.empty()) {
    // Show active services first, then others
    for (const auto& s : m_cachedServices) {
      if (s.activeState == "active") {
        results.push_back(createServiceResult(s, 200.0));
      }
    }
    for (const auto& s : m_cachedServices) {
      if (s.activeState != "active" && results.size() < kMaxResults) {
        results.push_back(createServiceResult(s, 100.0));
      }
    }
    return results;
  }

  const std::string lowerText = StringUtils::toLower(trimmed);
  struct ScoredService {
    const ServiceInfo* service = nullptr;
    double score = 0.0;
  };
  std::vector<ScoredService> scored;
  scored.reserve(m_cachedServices.size());

  for (const auto& s : m_cachedServices) {
    double score = FuzzyMatch::score(lowerText, s.cleanName);
    if (!FuzzyMatch::isMatch(score)) {
      if (s.cleanName.contains(lowerText) || s.unitName.contains(lowerText)) {
        score = 80.0;
      } else if (StringUtils::toLower(s.description).contains(lowerText)) {
        score = 40.0;
      } else {
        continue;
      }
    }

    if (s.cleanName.starts_with(lowerText)) {
      score += 150.0;
    }
    if (s.activeState == "active") {
      score += 50.0; // Boost active services
    }
    scored.push_back({.service = &s, .score = score});
  }

  std::ranges::sort(scored, [](const ScoredService& a, const ScoredService& b) { return a.score > b.score; });

  for (std::size_t i = 0; i < std::min(scored.size(), kMaxResults); ++i) {
    results.push_back(createServiceResult(*scored[i].service, scored[i].score));
  }

  return results;
}

std::vector<LauncherResult> ServicesProvider::queryPrefixed(std::string_view text) const { return query(text); }

LauncherResult ServicesProvider::createServiceResult(const ServiceInfo& s, double score) const {
  LauncherResult r;
  r.id = (s.isUser ? "user:" : "system:") + s.unitName;
  r.title = s.cleanName + " (" + s.unitName + ")";
  r.origin = s.isUser ? "User Service" : "System Service";
  r.originGlyph = "server";
  r.score = score;

  const bool isActive = (s.activeState == "active");
  const bool isFailed = (s.activeState == "failed");

  if (isActive) {
    r.badge = "●";
    r.glyphName = "player-play-filled";
    r.category = "Active";
  } else if (isFailed) {
    r.badge = "▲";
    r.glyphName = "alert-triangle";
    r.category = "Failed";
  } else {
    r.badge = "○";
    r.glyphName = "player-stop-filled";
    r.category = "Inactive";
  }

  std::string statusLine = (isActive ? "Active (running)" : (isFailed ? "Failed" : "Inactive (dead)"));
  if (!s.unitFileState.empty() && s.unitFileState != "unknown") {
    statusLine += " • " + s.unitFileState;
  }
  statusLine += " • " + (s.isUser ? std::string("User") : std::string("System"));
  if (!s.description.empty() && s.description != s.unitName) {
    statusLine += " • " + s.description;
  }
  r.subtitle = std::move(statusLine);

  return r;
}

LauncherResult ServicesProvider::createActionResult(std::string_view action, const ServiceInfo& s, double score) const {
  LauncherResult r;
  r.id = std::string(action) + ":" + (s.isUser ? "user:" : "system:") + s.unitName;
  r.title = std::string(action) + " " + s.unitName;
  r.subtitle = "Press Enter to " + std::string(action) + " " + s.cleanName + " (" + s.description + ")";
  r.origin = s.isUser ? "User Service" : "System Service";
  r.originGlyph = "player-play";
  r.glyphName = "settings";
  r.score = score;
  return r;
}

bool ServicesProvider::executeAction(std::string_view unitName, bool isUser, std::string_view action) const {
  kLog.info("Executing {} on {} (user={})", action, unitName, isUser);

  // Try via D-Bus first
  try {
    sdbus::IConnection* conn = nullptr;
    if (isUser && m_sessionBus != nullptr) {
      conn = &m_sessionBus->connection();
    } else if (!isUser && m_systemBus != nullptr) {
      conn = &m_systemBus->connection();
    }

    if (conn != nullptr) {
      auto proxy = sdbus::createProxy(*conn, kSystemdName, kSystemdPath);
      if (action == "start") {
        sdbus::ObjectPath job;
        proxy->callMethod("StartUnit")
            .onInterface(kSystemdManagerInterface)
            .withArguments(std::string{unitName}, "replace")
            .storeResultsTo(job);
        return true;
      }
      if (action == "stop") {
        sdbus::ObjectPath job;
        proxy->callMethod("StopUnit")
            .onInterface(kSystemdManagerInterface)
            .withArguments(std::string{unitName}, "replace")
            .storeResultsTo(job);
        return true;
      }
      if (action == "restart") {
        sdbus::ObjectPath job;
        proxy->callMethod("RestartUnit")
            .onInterface(kSystemdManagerInterface)
            .withArguments(std::string{unitName}, "replace")
            .storeResultsTo(job);
        return true;
      }
      if (action == "enable") {
        bool carriesInstallInfo = false;
        std::vector<sdbus::Struct<std::string, std::string, std::string>> changes;
        proxy->callMethod("EnableUnitFiles")
            .onInterface(kSystemdManagerInterface)
            .withArguments(std::vector<std::string>{std::string{unitName}}, false, true)
            .storeResultsTo(carriesInstallInfo, changes);
        return true;
      }
      if (action == "disable") {
        std::vector<sdbus::Struct<std::string, std::string, std::string>> changes;
        proxy->callMethod("DisableUnitFiles")
            .onInterface(kSystemdManagerInterface)
            .withArguments(std::vector<std::string>{std::string{unitName}}, false)
            .storeResultsTo(changes);
        return true;
      }
    }
  } catch (const sdbus::Error& e) {
    kLog.warn("D-Bus action {} on {} failed: {}", action, unitName, e.what());
  }

  // Fallback to systemctl command execution
  std::vector<std::string> args = {"systemctl"};
  if (isUser) {
    args.push_back("--user");
  }
  args.push_back(std::string(action));
  args.push_back(std::string(unitName));
  runSystemctlCommand(args);
  return true;
}

bool ServicesProvider::activate(const LauncherResult& result) {
  if (result.id.empty()) {
    return false;
  }

  // Check if it's an action result e.g. "start:system:bluetooth.service"
  for (std::string_view act : {"start", "stop", "restart", "enable", "disable"}) {
    const std::string prefix = std::string(act) + ":";
    if (result.id.starts_with(prefix)) {
      std::string_view rest = std::string_view(result.id).substr(prefix.size());
      const bool isUser = rest.starts_with("user:");
      if (isUser) {
        rest = rest.substr(5);
      } else if (rest.starts_with("system:")) {
        rest = rest.substr(7);
      }
      return executeAction(rest, isUser, act);
    }
  }

  // Normal service result: toggle active state
  std::string_view rest = result.id;
  const bool isUser = rest.starts_with("user:");
  if (isUser) {
    rest = rest.substr(5);
  } else if (rest.starts_with("system:")) {
    rest = rest.substr(7);
  }

  // Find service in cache to check current state
  std::lock_guard lock(m_mutex);
  auto it = std::ranges::find_if(m_cachedServices, [&](const ServiceInfo& s) {
    return s.unitName == rest && s.isUser == isUser;
  });

  if (it != m_cachedServices.end()) {
    const std::string action = (it->activeState == "active") ? "stop" : "start";
    return executeAction(rest, isUser, action);
  }

  return executeAction(rest, isUser, "start");
}

bool ServicesProvider::startService(std::string_view unitName, bool isUser) const {
  return executeAction(unitName, isUser, "start");
}

bool ServicesProvider::stopService(std::string_view unitName, bool isUser) const {
  return executeAction(unitName, isUser, "stop");
}

bool ServicesProvider::restartService(std::string_view unitName, bool isUser) const {
  return executeAction(unitName, isUser, "restart");
}

bool ServicesProvider::enableService(std::string_view unitName, bool isUser) const {
  return executeAction(unitName, isUser, "enable");
}

bool ServicesProvider::disableService(std::string_view unitName, bool isUser) const {
  return executeAction(unitName, isUser, "disable");
}

bool ServicesProvider::copyUnitName(std::string_view unitName) const {
  if (m_clipboard == nullptr) {
    return false;
  }
  return m_clipboard->copyText(std::string(unitName));
}
