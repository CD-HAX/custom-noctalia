#pragma once

#include "shell/bar/widget.h"
#include "system/icon_resolver.h"
#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/style.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class Box;
class Flex;
class Image;
class Label;
class InputArea;
class Renderer;
class CompositorPlatform;
class ConfigService;
struct wl_output;

class WorkspaceIdentifierWidget : public Widget {
public:
  struct Options {
    int iconSize = static_cast<int>(Style::baseGlyphSize);
    int maxLabelWidth = 140;
    bool showTitles = true;
    bool colorizeIcons = false;
  };

  WorkspaceIdentifierWidget(
      CompositorPlatform& platform, ConfigService& config, wl_output* output, const Options& options
  );
  ~WorkspaceIdentifierWidget() override;

  void create() override;
  bool wantsSecondTicks() const override { return true; }

private:
  struct WindowItem {
    std::string windowId;
    std::string appId;
    std::string title;
    bool isActive = false;
  };

  struct AppGroup {
    std::string appClass;
    std::string appName;
    std::string displayText;
    std::string iconPath;
    std::vector<WindowItem> windows;
    bool isActive = false;
  };

  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncWidgetVisibility(bool showWidget);
  void syncState(Renderer& renderer);
  void rebuild(Renderer& renderer);
  void focusWindow(const WindowItem& win);
  void activateAppGroup(const AppGroup& group);
  std::string resolveIconPath(const std::string& appId, float renderScale);
  void buildDesktopIconIndex();

  CompositorPlatform& m_platform;
  ConfigService& m_config;
  wl_output* m_output = nullptr;
  float m_iconSize = 16.0F;
  float m_maxLabelWidth = 140.0F;
  bool m_showTitles = true;
  bool m_colorizeIcons = false;

  bool m_isVertical = false;
  bool m_rebuildPending = true;
  std::uint64_t m_textMetricsGeneration = 0;
  std::uint64_t m_desktopEntriesVersion = 0;
  bool m_iconColorizeRefreshPending = false;
  Signal<>::ScopedConnection m_appIconColorizeConn;

  std::vector<AppGroup> m_groups;
  std::unordered_map<std::string, int> m_cycleIndexByApp;
  std::unordered_map<std::string, std::string> m_appIcons;
  IconResolver m_iconResolver;

  Flex* m_container = nullptr;
  InputArea* m_area = nullptr;
};
