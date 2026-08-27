#pragma once
#include <Epub.h>

#include <functional>
#include <cstdint>
#include <memory>
#include <optional>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"

/**
 * Activity for syncing reading progress with KOReader sync server.
 *
 * Flow:
 * 1. Connect to WiFi (if not connected)
 * 2. Calculate document hash
 * 3. Fetch remote progress
 * 4. Show comparison and options (Apply/Upload)
 * 5. Apply or upload progress
 */
class KOReaderSyncActivity final : public Activity, private UiAppHost {
 public:
  enum class Mode : uint8_t {
    MANUAL,
    AUTO_PULL,
    AUTO_PUSH,
  };

  enum class CompletionTarget : uint8_t {
    READER,
    HOME,
    FILE_BROWSER,
    SLEEP,
    SLEEP_TIMEOUT,
  };

  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                CrossPointPosition localPosition, SavedProgressPosition localKoPos,
                                std::string localChapterName, Mode mode = Mode::MANUAL,
                                CompletionTarget completionTarget = CompletionTarget::READER)
      : Activity("KOReaderSync", renderer, mappedInput),
        UiAppHost(renderer),
        epubPath(epubPath),
        localChapterName(std::move(localChapterName)),
        localPosition(localPosition),
        remoteProgress{},
        remotePosition{},
        localProgress(std::move(localKoPos)),
        mode(mode),
        completionTarget(completionTarget) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == CONNECTING || state == SYNCING || state == UPLOADING ||
           (automaticPull() && state == SHOWING_RESULT);
  }
  bool isReaderActivity() const override { return automaticMode(); }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    SYNC_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  std::shared_ptr<Epub> epub;  // null until lazy-loaded after TLS in performSync()
  std::string epubPath;
  std::string localChapterName;
  CrossPointPosition localPosition;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format (pre-computed before Epub was released)
  SavedProgressPosition localProgress;
  Mode mode;
  CompletionTarget completionTarget;
  uint32_t automaticOperationStartedAt = 0;

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // Timed return for successful smart-sync terminal states.
  unsigned long autoReturnAt = 0;
  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1200;

  // Tracks whether this session activated WiFi. Set in onEnter past the credentials
  // check; checked in onExit to decide whether to silent-reboot. Can't rely on
  // WiFi.getMode() because performUpload() calls esp_wifi_stop() on the way out,
  // which makes WiFi.getMode() return WIFI_MODE_NULL.
  bool wifiActivated = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
  bool smartSyncEnabled() const;
  bool automaticPull() const { return mode == Mode::AUTO_PULL; }
  bool automaticPush() const { return mode == Mode::AUTO_PUSH; }
  bool automaticMode() const { return mode != Mode::MANUAL; }
  uint32_t automaticOperationRemainingMs() const;
  bool automaticOperationDeadlineExpired() const;
  KOReaderSyncClient::Error getProgress(const std::string& hash, KOReaderProgress& progress);
  KOReaderSyncClient::Error updateProgress(const KOReaderProgress& progress);
  void markAutoReturn();
  void completeAlreadySynced();
  void ensureEpubLoaded();
  void saveProgressAndReturn(int spineIndex, int page);
  void returnToReader();
  void completeFlow();

  // The UiAppHost app hosts the interactive states (SHOWING_RESULT compare
  // rows and the NO_REMOTE_PROGRESS upload prompt) so they get themed
  // rows/buttons and tap-flash; the header stays on GUI.drawHeader for the
  // battery indicator and the purely-informational states keep their raw
  // centered text.
  static void resultScreen(UiScreen& screen, void* user);
  static void onResultRow(const freeink::ui::ActionEvent& event, void* user);
  void buildResultScreen(UiScreen& screen);
  void chooseResultOption();
  void startUpload();
};
