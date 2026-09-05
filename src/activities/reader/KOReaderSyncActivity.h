#pragma once
#include <Epub.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "KOReaderSyncClient.h"
#include "KOReaderSyncSession.h"
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
  enum class Mode : uint8_t { MANUAL, AUTOMATIC };
  enum class CompletionTarget : uint8_t { READER, HOME, SLEEP, BOOK_SWITCH };

  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                int currentSpineIndex, int currentPage, int totalPagesInSpine,
                                SavedProgressPosition localKoPos, CrossPointPosition localPosition,
                                std::optional<KOReaderMetadata> localMetadata, std::string localChapterName,
                                std::optional<uint16_t> currentParagraphIndex = std::nullopt, Mode mode = Mode::MANUAL,
                                KOReaderSyncTrigger trigger = KOReaderSyncTrigger::Open,
                                CompletionTarget completionTarget = CompletionTarget::READER,
                                std::string continuationPath = {});

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == CONNECTING || state == SYNCING || state == UPLOADING ||
           (mode == Mode::AUTOMATIC && state == SHOWING_RESULT);
  }

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
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;
  std::optional<uint16_t> currentParagraphIndex;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format (pre-computed before Epub was released)
  SavedProgressPosition localProgress;
  CrossPointPosition localPosition;
  std::optional<KOReaderMetadata> localMetadata;
  bool remotePositionCanBeApplied = false;

  Mode mode = Mode::MANUAL;
  KOReaderSyncTrigger trigger = KOReaderSyncTrigger::Open;
  CompletionTarget completionTarget = CompletionTarget::READER;
  uint32_t automaticStartedAt = 0;
  std::unique_ptr<KOReaderSyncHttpSession> automaticClient;
  std::string continuationPath;

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
  bool wifiWasAlreadyConnected = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performAutomaticSync();
  void performUpload();
  bool smartSyncEnabled() const;
  void markAutoReturn();
  void completeAlreadySynced();
  void ensureEpubLoaded();
  void saveProgressAndReturn(int spineIndex, int page);
  void returnToReader();
  void completeFlow(const char* reason = "finished");
  KOReaderProgress buildLocalProgress() const;
  bool mapAutomaticRemoteProgress();
  KOReaderSyncComparison compareAutomaticPositions() const;
  void beginAutomaticTransaction();
  void applyAutomaticLatestRemote();
  void handleAutomaticOutcome(KOReaderSyncOutcome outcome);

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
