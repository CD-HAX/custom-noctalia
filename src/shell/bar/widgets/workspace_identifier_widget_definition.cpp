#include "shell/bar/widgets/workspace_identifier_widget_definition.h"

const noctalia::bar::WidgetDefinition<WorkspaceIdentifierWidget::Options>& workspaceIdentifierWidgetDefinition() {
  using noctalia::bar::field;
  using Options = WorkspaceIdentifierWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "workspace_identifier",
      .fields = {
          field<&Options::colorizeIcons>({
              .key = "colorize_icons",
          }),
          field<&Options::iconSize>({
              .key = "icon_size",
              .minValue = 8.0,
              .maxValue = 64.0,
              .step = 1.0,
          }),
          field<&Options::maxLabelWidth>({
              .key = "max_label_width",
              .minValue = 40.0,
              .maxValue = 600.0,
              .step = 1.0,
          }),
          field<&Options::showTitles>({
              .key = "show_titles",
          }),
      },
  };
  return definition;
}
