#include "launcher/services_provider.h"
#include "tests/test_check.h"

int main() {
  ServicesProvider provider(nullptr, nullptr, nullptr);
  provider.initialize();
  provider.onLauncherOpened();

  TEST_CHECK(provider.id() == "services");
  TEST_CHECK(provider.defaultPrefix() == "services");
  TEST_CHECK(!provider.defaultIncludeInGlobalSearch());
  TEST_CHECK(!provider.categories().empty());

  // Action queries generate direct action launcher results
  const auto startResults = provider.queryPrefixed("start demo.service");
  TEST_CHECK(!startResults.empty());
  TEST_CHECK(startResults.front().id.starts_with("start:"));

  const auto stopResults = provider.queryPrefixed("stop demo.service");
  TEST_CHECK(!stopResults.empty());
  TEST_CHECK(stopResults.front().id.starts_with("stop:"));

  const auto restartResults = provider.queryPrefixed("restart demo.service");
  TEST_CHECK(!restartResults.empty());
  TEST_CHECK(restartResults.front().id.starts_with("restart:"));

  // Copy unit name returns false when clipboard is null
  TEST_CHECK(!provider.copyUnitName("demo.service"));

  return 0;
}
