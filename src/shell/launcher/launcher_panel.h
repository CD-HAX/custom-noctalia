#pragma once

#include "core/timer_manager.h"
#include "launcher/launcher_provider.h"
#include "launcher/usage_tracker.h"
#include "render/core/texture_handle.h"
#include "shell/panel/panel.h"
#include "system/icon_resolver.h"
#include "ui/signal.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ContextMenuPopup;
class AccountsService;
class Box;
class Button;
class Flex;
class Glyph;
class Image;
class Input;
class Label;
class LauncherResultAdapter;
class LauncherAppGridAdapter;
class LauncherSectionsAdapter;
class Renderer;
class Segmented;
class ScrollView;
class VirtualGridView;
class VirtualListView;
class ConfigService;
class AsyncTextureCache;

class LauncherPanel : public Panel {
public:
  LauncherPanel(ConfigService* config, AccountsService* accounts, AsyncTextureCache* asyncTextures);
  ~LauncherPanel() override;

  void addProvider(std::unique_ptr<LauncherProvider> provider);
  // Drop every dynamically-registered (plugin-backed) provider, so the enabled
  // plugin set can be re-applied without disturbing the built-in providers.
  void clearDynamicProviders();
  // Drop providers whose stable id starts with `prefix` (e.g. config-driven "dmenu.").
  void clearProvidersWithIdPrefix(std::string_view prefix);
  // Restrict the next open to a single provider (stdin/dmenu session). When set,
  // onInputChanged queries only that provider and skips prefix routing/overview.
  // Cleared on close.
  void setScopedProvider(std::string_view providerId, std::string_view placeholder = {});

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onIconThemeChanged() override;

  void clearUsage();
  void syncUsageOrderingState();

  // Invoked after a terminal close when the activation copied text and the provider
  // supports auto-paste. The host schedules virtual-keyboard paste (clipboard path).
  void setCopiedActivationCallback(std::function<void()> callback) { m_onCopiedActivation = std::move(callback); }

  void setOnAppLaunchCallback(std::function<void()> callback) { m_onAppLaunch = std::move(callback); }

  float preferredWidth() const override;
  float preferredHeight() const override;
  LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  InputArea* initialFocusArea() const override;
  bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  PanelPlacement panelPlacement() const noexcept override;
  bool ignorePointerEvent() const override;

private:
  friend class LauncherPanelTestAccess;

  enum ActiveCategoryType { All, Pinned, RecentlyUsed, Category };
  enum class ResultsTransition : std::uint8_t { None, NavigationForward, NavigationBackward };

  struct CategoryFilterSlot {
    ActiveCategoryType type;
    std::size_t categoryIndex = 0;
  };

  struct BrowserViewState {
    ActiveCategoryType categoryType = All;
    std::size_t categorySlotIndex = 0;
    std::string category;
    std::string query;
    std::string providerViewId;
  };

  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void onInputChanged(const std::string& text);
  void setQuery(const std::string& query);
  // Re-gather the current query, preserving the selected result by identity.
  void reapplyCurrentQuery();
  // A plugin provider delivered fresh async results — re-gather if the panel is open.
  void onProviderResultsChanged();
  void refreshResults();
  void activateAt(std::size_t index);
  void activateSelected();
  bool handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  bool cycleCategories(int direction);
  void applyEmptyState();
  void bindDetailResult();
  bool shouldUseDetailPresentation() const;
  bool startsWithLauncherPrefix(std::string_view text) const;
  void applyProviderConfig(LauncherProvider& provider) const;
  void finishActivation(LauncherProvider& provider, const std::string& resultId, bool copied, bool appLaunch = false);
  std::vector<LauncherResult> providerOverviewResults(std::string_view text) const;
  [[nodiscard]] bool openAppActionsMenu(std::size_t index, float anchorX, float anchorY);
  [[nodiscard]] bool openFileActionsMenu(const LauncherResult& base, float anchorX, float anchorY);
  [[nodiscard]] bool openServiceActionsMenu(const LauncherResult& base, float anchorX, float anchorY);
  void activateCalculatorResult();
  void updateCategoryFilterModel(const std::vector<LauncherCategory>& categories);
  void rebuildCategoryFilter();
  void rebuildLauncherNavigation();
  ActiveCategoryType defaultCategoryType() const;
  void applyDefaultView();
  void updateCategorySelectionChrome();
  void updateLauncherOptionButtons();
  void setCategoryFilterVisible(bool visible);
  void setActiveCategorySlot(std::size_t slotIndex);
  void setActiveBrowserCategory(std::size_t categoryIndex);
  void setActiveProviderCategory(std::size_t index);
  void applyActiveCategory();
  void syncLauncherListStyle();
  void syncLauncherViewLayout(Renderer* renderer = nullptr);
  void queueResultsTransition(ResultsTransition transition);
  void animateResultsTransition();
  bool shouldUseAppGrid() const;
  bool supportsResultGrid() const;
  bool shouldUseLauncherSections() const;
  void rebuildLauncherSections();
  void selectResult(std::size_t index, bool scrollToSelection);
  void refreshLauncherAppIconColorization();
  void updateLauncherGridMetrics(Renderer& renderer);
  void updatePinnedApplicationState();
  void applyPinnedApplicationOrder();
  void reorderPinnedApplication(std::string_view sourcePath, std::string_view targetPath);
  void syncLauncherAvatar(Renderer& renderer);
  void syncDetailImage(Renderer& renderer);
  void showProviderOverview();
  void showSessionActions();
  bool usesBrowserLayout() const;

  std::vector<std::unique_ptr<LauncherProvider>> m_providers;
  std::vector<LauncherResult> m_results;
  std::vector<LauncherResult> m_allResults;
  UsageTracker m_usageTracker;
  IconResolver m_iconResolver;

  Flex* m_container = nullptr;
  Input* m_input = nullptr;
  Segmented* m_categoryFilter = nullptr;
  Flex* m_body = nullptr;
  Flex* m_resultsViewport = nullptr;
  Flex* m_launcherToolbar = nullptr;
  Segmented* m_launcherNavigation = nullptr;
  Image* m_launcherAvatar = nullptr;
  Glyph* m_launcherAvatarFallback = nullptr;
  Button* m_launcherSettingsButton = nullptr;
  Button* m_launcherUsageButton = nullptr;
  Button* m_launcherGridButton = nullptr;
  Button* m_launcherPowerButton = nullptr;
  Button* m_calcPill = nullptr;
  VirtualGridView* m_grid = nullptr;
  VirtualListView* m_launcherSections = nullptr;
  ScrollView* m_detailScroll = nullptr;
  Image* m_detailImage = nullptr;
  std::string m_detailImagePath;
  std::string m_lastSyncedDetailPath;
  std::string m_inspectedResultId;

  struct AnimatedGifFrame {
    TextureHandle handle{};
    std::chrono::milliseconds duration{0};
  };

  bool tryLoadDetailGif(Renderer& renderer, const std::string& path);
  void scheduleNextGifFrame();
  void onGifFrameTimer();
  void stopGifAnimation();

  std::vector<AnimatedGifFrame> m_detailGifFrames;
  std::size_t m_detailGifCurrentFrame = 0;
  Timer m_detailGifTimer;
  std::string m_currentGifPath;
  Renderer* m_renderer = nullptr;

  Label* m_detailSubtitle = nullptr;
  Label* m_detailBody = nullptr;
  Label* m_emptyLabel = nullptr;

  // Files browsing & inspector controls
  Flex* m_filesLeftPane = nullptr;
  Flex* m_filesNavHeader = nullptr;
  Button* m_filesUpButton = nullptr;
  Label* m_filesPathLabel = nullptr;
  Box* m_filesCenterDivider = nullptr;

  Flex* m_detailFolderBox = nullptr;
  Glyph* m_detailFolderGlyph = nullptr;
  Flex* m_detailImageBox = nullptr;
  Flex* m_detailBinaryBox = nullptr;
  Glyph* m_detailFileGlyph = nullptr;
  Label* m_detailBinaryHint = nullptr;
  Box* m_detailDivider = nullptr;
  Flex* m_detailMetaTable = nullptr;
  Flex* m_metaRowName = nullptr;
  Label* m_metaValueName = nullptr;
  Flex* m_metaRowType = nullptr;
  Label* m_metaValueType = nullptr;
  Flex* m_metaRowSize = nullptr;
  Label* m_metaValueSize = nullptr;
  Flex* m_metaRowPath = nullptr;
  Label* m_metaValuePath = nullptr;
  Flex* m_metaRowItems = nullptr;
  Label* m_metaValueItems = nullptr;
  Flex* m_metaRowLines = nullptr;
  Label* m_metaValueLines = nullptr;
  Flex* m_metaRowChars = nullptr;
  Label* m_metaValueChars = nullptr;

  bool isFilesView() const;
  void updateFilesNavHeader();
  void bindFilesInspector(const LauncherResult& result);
  void filesGoUp();
  void reapplyQuery();

  bool m_anyProviderLoading = false;
  std::unique_ptr<LauncherResultAdapter> m_listAdapter;
  std::unique_ptr<LauncherAppGridAdapter> m_gridAdapter;
  std::unique_ptr<LauncherSectionsAdapter> m_launcherSectionsAdapter;

  std::string m_query;
  std::string m_resultProviderId;
  std::string m_scopedProviderId;
  std::string m_scopedPlaceholder;
  ActiveCategoryType m_activeCategoryType = All;
  std::string m_activeCategory;
  std::vector<LauncherCategory> m_currentCategories;
  std::vector<LauncherCategory> m_providerCategories;
  std::string m_activeProviderCategory;
  std::vector<CategoryFilterSlot> m_categoryFilterSlots;
  bool m_hasRecentlyUsed = false;
  bool m_hasPinnedApplications = false;
  bool m_categoryFilterRebuildPending = false;
  bool m_applyDefaultViewPending = false;
  bool m_syncingLauncherNavigation = false;
  // Temporary reveal when Simple layout has shell.launcher.categories disabled.
  bool m_forceCategoryFilterVisible = false;
  std::size_t m_selectedIndex = 0;
  std::size_t m_activeCategorySlotIndex = 0;
  bool m_browserMode = false;
  bool m_showingProviderOverview = false;
  std::string m_launcherProviderViewId;
  std::optional<BrowserViewState> m_sessionReturnView;
  bool m_launcherShowIcons = true;
  bool m_launcherShowAppOriginIndicator = true;
  bool m_launcherCompact = false;
  bool m_launcherAppGrid = false;
  bool m_launcherListItemBackground = false;
  bool m_usingAppGrid = false;
  ResultsTransition m_pendingResultsTransition = ResultsTransition::None;
  bool m_resultsTransitionActive = false;
  float m_resultsTransitionBaseX = 0.0F;
  float m_launcherRowHeight = 0.0F;
  std::uint64_t m_desktopEntriesVersion = 0;
  ConfigService* m_config = nullptr;
  AccountsService* m_accounts = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  std::string m_launcherAvatarPath;
  std::unique_ptr<ContextMenuPopup> m_actionsMenu;
  Signal<>::ScopedConnection m_appIconColorizeConn;
  std::function<void()> m_onCopiedActivation;
  std::function<void()> m_onAppLaunch;
};
