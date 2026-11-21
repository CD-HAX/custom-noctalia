#include "shell/bar/widgets/workspace_identifier_widget.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/app_icon_colorization_settings.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

WorkspaceIdentifierWidget::WorkspaceIdentifierWidget(
    CompositorPlatform& platform, ConfigService& config, wl_output* output, const Options& options
)
    : m_platform(platform), m_config(config), m_output(output), m_iconSize(static_cast<float>(options.iconSize)),
      m_maxLabelWidth(static_cast<float>(options.maxLabelWidth)), m_showTitles(options.showTitles),
      m_colorizeIcons(options.colorizeIcons) {
  buildDesktopIconIndex();
}

WorkspaceIdentifierWidget::~WorkspaceIdentifierWidget() = default;

void WorkspaceIdentifierWidget::create() {
  auto rootArea = ui::inputArea({});
  m_area = rootArea.get();

  auto container = ui::flex(
      m_isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
      {
          .align = FlexAlign::Center,
          .gap = 8.0F * m_contentScale,
      }
  );
  m_container = container.get();
  rootArea->addChild(std::move(container));
  setRoot(std::move(rootArea));

  m_appIconColorizeConn = shellAppIconColorizationChanged().connect([this] {
    m_iconColorizeRefreshPending = true;
    requestUpdate();
  });
}

void WorkspaceIdentifierWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  const bool wasVertical = m_isVertical;
  m_isVertical = containerHeight > containerWidth;
  if (wasVertical != m_isVertical && m_container != nullptr) {
    m_container->setDirection(m_isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal);
    m_rebuildPending = true;
  }
  const std::uint64_t textMetricsGeneration = renderer.textMetricsGeneration();
  if (m_textMetricsGeneration != textMetricsGeneration) {
    m_textMetricsGeneration = textMetricsGeneration;
    m_rebuildPending = true;
  }
  syncState(renderer);
  if (m_rebuildPending) {
    rebuild(renderer);
    m_rebuildPending = false;
  } else if (m_container != nullptr) {
    m_container->layout(renderer);
    if (m_area != nullptr) {
      m_area->setSize(m_container->width(), m_container->height());
    }
  }
}

void WorkspaceIdentifierWidget::doUpdate(Renderer& renderer) {
  if (m_iconColorizeRefreshPending) {
    m_iconColorizeRefreshPending = false;
    m_rebuildPending = true;
  }
  syncState(renderer);
  if (m_rebuildPending) {
    rebuild(renderer);
    m_rebuildPending = false;
    if (root() != nullptr) {
      root()->markPaintDirty();
    }
  } else if (m_container != nullptr) {
    m_container->layout(renderer);
    if (m_area != nullptr) {
      m_area->setSize(m_container->width(), m_container->height());
    }
  }
}

void WorkspaceIdentifierWidget::syncWidgetVisibility(bool showWidget) {
  if (Node* rootNode = root(); rootNode != nullptr) {
    if (rootNode->visible() != showWidget || rootNode->participatesInLayout() != showWidget) {
      rootNode->setVisible(showWidget);
      rootNode->setParticipatesInLayout(showWidget);
      if (!showWidget) {
        rootNode->setSize(0.0F, 0.0F);
      }
      requestUpdate();
    } else if (!showWidget && (rootNode->width() > 0.0F || rootNode->height() > 0.0F)) {
      rootNode->setSize(0.0F, 0.0F);
    }
  }
}

void WorkspaceIdentifierWidget::buildDesktopIconIndex() {
  m_appIcons.clear();
  auto addIndexKey = [this](std::string_view key, const std::string& icon) {
    if (key.empty() || icon.empty()) {
      return;
    }
    m_appIcons.try_emplace(std::string{key}, icon);
    m_appIcons.try_emplace(StringUtils::toLower(key), icon);
  };

  const auto& entries = desktopEntries();
  for (const auto& entry : entries) {
    if (entry.id.empty() || entry.icon.empty()) {
      continue;
    }

    addIndexKey(entry.id, entry.icon);
    if (const auto dot = entry.id.rfind('.'); dot != std::string::npos && dot + 1 < entry.id.size()) {
      addIndexKey(entry.id.substr(dot + 1), entry.icon);
    }
    if (const auto dash = entry.id.rfind('-'); dash != std::string::npos && dash + 1 < entry.id.size()) {
      const std::string suffix = entry.id.substr(dash + 1);
      if (suffix == "bin" || suffix == "desktop") {
        addIndexKey(entry.id.substr(0, dash), entry.icon);
      }
    }
    if (!entry.startupWmClass.empty()) {
      addIndexKey(entry.startupWmClass, entry.icon);
    }
  }
  m_desktopEntriesVersion = desktopEntriesVersion();
}

std::string WorkspaceIdentifierWidget::resolveIconPath(const std::string& appId, float renderScale) {
  if (appId.empty()) {
    return {};
  }

  const int iconLookupSize = std::max(1, static_cast<int>(std::round(m_iconSize * std::max(1.0F, renderScale))));

  if (const auto entry = app_identity::findDesktopEntry(appId, desktopEntries());
      entry.has_value() && !entry->icon.empty()) {
    const std::string& resolved = m_iconResolver.resolve(entry->icon, iconLookupSize);
    if (!resolved.empty()) {
      return resolved;
    }
  }

  auto resolveByName = [this, iconLookupSize](const std::string& name) -> std::string {
    if (name.empty()) {
      return {};
    }
    return m_iconResolver.resolve(name, iconLookupSize);
  };

  if (auto it = m_appIcons.find(appId); it != m_appIcons.end()) {
    const auto path = resolveByName(it->second);
    if (!path.empty()) {
      return path;
    }
  }

  const std::string appIdLower = StringUtils::toLower(appId);
  if (auto it = m_appIcons.find(appIdLower); it != m_appIcons.end()) {
    const auto path = resolveByName(it->second);
    if (!path.empty()) {
      return path;
    }
  }

  if (const auto slash = appId.find_last_of('/'); slash != std::string::npos && slash + 1 < appId.size()) {
    const std::string tail = appId.substr(slash + 1);
    if (auto it = m_appIcons.find(tail); it != m_appIcons.end()) {
      const auto path = resolveByName(it->second);
      if (!path.empty()) {
        return path;
      }
    }
  }

  return resolveByName(appId);
}

void WorkspaceIdentifierWidget::syncState(Renderer& /*renderer*/) {
  const auto desktopVersion = desktopEntriesVersion();
  if (desktopVersion != m_desktopEntriesVersion) {
    buildDesktopIconIndex();
  }

  auto workspaces = m_platform.workspaces(m_output);
  if (workspaces.empty()) {
    workspaces = m_platform.workspaces();
  }

  const Workspace* activeWs = nullptr;
  for (const auto& ws : workspaces) {
    if (ws.active) {
      activeWs = &ws;
      break;
    }
  }

  if (activeWs == nullptr) {
    const auto allWorkspaces = m_platform.workspaces();
    for (const auto& ws : allWorkspaces) {
      if (ws.active) {
        activeWs = &ws;
        break;
      }
    }
  }

  auto assignments = m_platform.workspaceWindowAssignments(m_output);
  if (assignments.empty()) {
    assignments = m_platform.workspaceWindowAssignments();
  }

  const auto focusedWindowId = m_platform.focusedCompositorWindowId();
  const auto activeToplevel = m_platform.activeToplevel();

  auto matchesActiveWorkspace = [&](const std::string& wsKey) {
    if (wsKey.empty()) {
      return false;
    }
    if (activeWs == nullptr) {
      return true;
    }
    if (wsKey == activeWs->id || wsKey == activeWs->name) {
      return true;
    }
    if (wsKey == std::to_string(activeWs->index)) {
      return true;
    }
    if (wsKey.ends_with("/" + activeWs->id) || wsKey.ends_with(":" + activeWs->id)) {
      return true;
    }
    if (wsKey.ends_with("/" + activeWs->name) || wsKey.ends_with(":" + activeWs->name)) {
      return true;
    }
    if (activeWs->id.ends_with("/" + wsKey) || activeWs->id.ends_with(":" + wsKey)) {
      return true;
    }
    return false;
  };

  std::vector<WindowItem> rawList;
  for (const auto& assignment : assignments) {
    if (!matchesActiveWorkspace(assignment.workspaceKey)) {
      continue;
    }
    std::string appId = assignment.appId;
    if (appId.empty() && assignment.title.empty()) {
      continue;
    }
    if (appId.empty()) {
      appId = "window";
    }

    bool isActive = false;
    if (focusedWindowId.has_value() && !assignment.windowId.empty()) {
      const std::string_view f = *focusedWindowId;
      const std::string_view w = assignment.windowId;
      if (f == w || (f.starts_with("0x") && f.substr(2) == w) || (w.starts_with("0x") && w.substr(2) == f)) {
        isActive = true;
      }
    }
    if (!isActive && activeToplevel.has_value()) {
      if (!activeToplevel->identifier.empty() && activeToplevel->identifier == assignment.windowId) {
        isActive = true;
      } else if (
          !activeToplevel->title.empty()
          && activeToplevel->title == assignment.title
          && !activeToplevel->appId.empty()
          && StringUtils::toLower(activeToplevel->appId) == StringUtils::toLower(appId)
      ) {
        isActive = true;
      }
    }

    rawList.push_back(
        WindowItem{
            .windowId = assignment.windowId,
            .appId = std::move(appId),
            .title = assignment.title,
            .isActive = isActive,
        }
    );
  }

  if (rawList.empty() && activeToplevel.has_value()) {
    wl_output* topOutput = m_platform.activeToplevelOutput();
    if (topOutput == nullptr || m_output == nullptr || topOutput == m_output) {
      rawList.push_back(
          WindowItem{
              .windowId = activeToplevel->identifier,
              .appId = !activeToplevel->appId.empty() ? activeToplevel->appId : "window",
              .title = activeToplevel->title,
              .isActive = true,
          }
      );
    }
  }

  if (!rawList.empty() && !std::ranges::any_of(rawList, [](const WindowItem& w) { return w.isActive; })) {
    rawList[0].isActive = true;
  }

  std::vector<AppGroup> nextGroups;
  std::unordered_map<std::string, std::size_t> groupIndexByApp;

  for (auto& item : rawList) {
    const std::string appKey = StringUtils::toLower(item.appId);
    auto it = groupIndexByApp.find(appKey);
    if (it == groupIndexByApp.end()) {
      std::string appName = item.appId;
      if (const auto entry = app_identity::findDesktopEntry(item.appId, desktopEntries());
          entry.has_value() && !entry->name.empty()) {
        appName = entry->name;
      } else {
        std::string base = item.appId;
        if (const auto dot = base.rfind('.'); dot != std::string::npos) {
          base = base.substr(dot + 1);
        }
        if (!base.empty()) {
          base[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(base[0])));
        }
        appName = std::move(base);
      }

      AppGroup group;
      group.appClass = item.appId;
      group.appName = appName;
      group.iconPath = resolveIconPath(item.appId, m_contentScale);
      if (item.isActive) {
        group.isActive = true;
      }
      group.windows.push_back(std::move(item));

      groupIndexByApp[appKey] = nextGroups.size();
      nextGroups.push_back(std::move(group));
    } else {
      auto& group = nextGroups[it->second];
      if (item.isActive) {
        group.isActive = true;
      }
      group.windows.push_back(std::move(item));
    }
  }

  const bool moreThanTwoApps = nextGroups.size() > 2;
  for (auto& group : nextGroups) {
    const std::size_t count = group.windows.size();
    if (count > 1) {
      group.displayText = std::format("{} +{}", group.appName, count);
    } else {
      if (!moreThanTwoApps && m_showTitles && !group.windows[0].title.empty()) {
        group.displayText = group.windows[0].title;
      } else {
        group.displayText = group.appName;
      }
    }
  }

  bool changed = nextGroups.size() != m_groups.size();
  if (!changed) {
    for (std::size_t i = 0; i < nextGroups.size(); ++i) {
      if (nextGroups[i].appClass != m_groups[i].appClass
          || nextGroups[i].displayText != m_groups[i].displayText
          || nextGroups[i].isActive != m_groups[i].isActive
          || nextGroups[i].windows.size() != m_groups[i].windows.size()
          || nextGroups[i].iconPath != m_groups[i].iconPath) {
        changed = true;
        break;
      }
    }
  }

  if (changed) {
    m_groups = std::move(nextGroups);
    m_rebuildPending = true;
  }
}

void WorkspaceIdentifierWidget::focusWindow(const WindowItem& win) {
  if (!win.windowId.empty()) {
    m_platform.focusCompositorWindow(win.windowId, false);
  } else if (!win.appId.empty()) {
    m_platform.activateToplevelForAppId(win.appId);
  }
}

void WorkspaceIdentifierWidget::activateAppGroup(const AppGroup& group) {
  if (group.windows.empty()) {
    return;
  }
  if (group.windows.size() == 1) {
    focusWindow(group.windows[0]);
    return;
  }

  int currentIdx = -1;
  const auto focusedWindowId = m_platform.focusedCompositorWindowId();
  for (std::size_t i = 0; i < group.windows.size(); ++i) {
    if (focusedWindowId.has_value() && !group.windows[i].windowId.empty()) {
      const std::string_view f = *focusedWindowId;
      const std::string_view w = group.windows[i].windowId;
      if (f == w || (f.starts_with("0x") && f.substr(2) == w) || (w.starts_with("0x") && w.substr(2) == f)) {
        currentIdx = static_cast<int>(i);
        break;
      }
    }
  }
  if (currentIdx < 0) {
    for (std::size_t i = 0; i < group.windows.size(); ++i) {
      if (group.windows[i].isActive) {
        currentIdx = static_cast<int>(i);
        break;
      }
    }
  }
  if (currentIdx < 0) {
    if (auto it = m_cycleIndexByApp.find(group.appClass); it != m_cycleIndexByApp.end()) {
      currentIdx = it->second;
    }
  }
  const int nextIdx = (currentIdx + 1) % static_cast<int>(group.windows.size());
  m_cycleIndexByApp[group.appClass] = nextIdx;
  focusWindow(group.windows[static_cast<std::size_t>(nextIdx)]);
}

void WorkspaceIdentifierWidget::rebuild(Renderer& renderer) {
  if (m_container == nullptr) {
    return;
  }

  while (!m_container->children().empty()) {
    m_container->removeChild(m_container->children().back().get());
  }

  if (m_groups.empty()) {
    syncWidgetVisibility(false);
    return;
  }
  syncWidgetVisibility(true);

  const float scale = m_contentScale;
  const float iconSz = std::round(m_iconSize * scale);

  for (const auto& group : m_groups) {
    std::string tip;
    if (group.windows.size() == 1) {
      tip = !group.windows[0].title.empty() ? group.windows[0].title : group.appName;
    } else {
      tip = group.appName + std::format(" ({} windows):\n", group.windows.size());
      for (std::size_t i = 0; i < group.windows.size(); ++i) {
        if (i > 0) {
          tip += "\n";
        }
        tip += std::format("{}. {}", i + 1, !group.windows[i].title.empty() ? group.windows[i].title : group.appName);
      }
    }

    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = 6.0F * scale,
    });

    if (!group.iconPath.empty()) {
      auto img = ui::image({
          .fit = ImageFit::Contain,
          .width = iconSz,
          .height = iconSz,
      });
      img->setAppIconColorization(effectiveShellAppIconColorizationTint(m_config.config().shell, m_colorizeIcons));
      (void)img->setSourceFile(renderer, group.iconPath, static_cast<int>(iconSz), false);
      row->addChild(std::move(img));
    } else {
      row->addChild(
          ui::glyph({
              .glyph = "app-window",
              .glyphSize = iconSz,
              .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
          })
      );
    }

    if (!m_isVertical) {
      row->addChild(
          ui::label({
              .text = group.displayText,
              .fontSize = Style::fontSizeBody * fontScale(),
              .fontWeight = labelFontWeight(),
              .fontFamily = labelFontFamily(),
              .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
              .maxWidth = m_maxLabelWidth * scale,
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
          })
      );
    }

    if (m_groups.size() == 1 && m_area != nullptr) {
      m_area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
      m_area->setTooltip(std::move(tip));
      m_area->setOnClick([this, appClass = group.appClass](const InputArea::PointerData& data) {
        if (data.button == BTN_LEFT) {
          for (const auto& g : m_groups) {
            if (g.appClass == appClass) {
              activateAppGroup(g);
              break;
            }
          }
        }
      });
    } else {
      auto itemArea = ui::inputArea({});
      itemArea->setParticipatesInLayout(false);
      itemArea->setZIndex(1);
      itemArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
      itemArea->setTooltip(std::move(tip));
      itemArea->setOnClick([this, appClass = group.appClass](const InputArea::PointerData& data) {
        if (data.button == BTN_LEFT) {
          for (const auto& g : m_groups) {
            if (g.appClass == appClass) {
              activateAppGroup(g);
              break;
            }
          }
        }
      });
      row->addChild(std::move(itemArea));
    }

    m_container->addChild(std::move(row));
  }

  m_container->layout(renderer);
  if (m_groups.size() > 1) {
    if (m_area != nullptr) {
      m_area->setOnClick(nullptr);
      m_area->setTooltip("");
    }
    for (const auto& child : m_container->children()) {
      if (child->children().size() > 1) {
        if (auto* area = dynamic_cast<InputArea*>(child->children().back().get())) {
          area->setPosition(0.0F, 0.0F);
          area->setFrameSize(child->width(), child->height());
        }
      }
    }
  }
  if (m_area != nullptr) {
    m_area->setSize(m_container->width(), m_container->height());
  }
}
