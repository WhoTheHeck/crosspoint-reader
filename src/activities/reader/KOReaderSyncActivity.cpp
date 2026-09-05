#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>
#include <cmath>

#include "DeepSleep.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "Memory.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"  // list icons for the compare rows
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
// One action id for both interactive states: the compare rows (SHOWING_RESULT)
// and the upload button (NO_REMOTE_PROGRESS) never coexist, so state
// disambiguates them in the handler.
constexpr fui::ActionId ACTION_ROW = 1;

std::string calculateDocumentHashForMethod(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path);
}

DocumentMatchMethod alternateMatchMethod(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
}

const char* matchMethodName(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? "filename" : "binary";
}

}  // namespace

KOReaderSyncActivity::KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& epubPath, int currentSpineIndex, int currentPage,
                                           int totalPagesInSpine, SavedProgressPosition localKoPos,
                                           CrossPointPosition localPosition,
                                           std::optional<KOReaderMetadata> localMetadata, std::string localChapterName,
                                           std::optional<uint16_t> currentParagraphIndex, const Mode mode,
                                           const KOReaderSyncTrigger trigger, const CompletionTarget completionTarget,
                                           std::string continuationPath)
    : Activity("KOReaderSync", renderer, mappedInput),
      UiAppHost(renderer),
      epubPath(epubPath),
      localChapterName(std::move(localChapterName)),
      currentSpineIndex(currentSpineIndex),
      currentPage(currentPage),
      totalPagesInSpine(totalPagesInSpine),
      currentParagraphIndex(currentParagraphIndex),
      remoteProgress{},
      remotePosition{},
      localProgress(std::move(localKoPos)),
      localPosition(localPosition),
      localMetadata(std::move(localMetadata)),
      mode(mode),
      trigger(trigger),
      completionTarget(completionTarget),
      automaticStartedAt(millis()),
      continuationPath(std::move(continuationPath)) {}

void KOReaderSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    LOG_DBG("KOSync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
    epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    epub->setupCacheDir();
    // Load metadata only (no CSS needed for progress mapping, don't rebuild if cache is missing).
    if (!epub->load(false, true)) {
      LOG_ERR("KOSync", "Failed to load epub for progress mapping");
      epub.reset();
      return;
    }
    LOG_DBG("KOSync", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  }
}

void KOReaderSyncActivity::saveProgressAndReturn(int spineIndex, int page) {
  // epub is guaranteed non-null here: ensureEpubLoaded() was called in performSync() before
  // SHOWING_RESULT state is entered, and this method is only called from that state.
  assert(epub);
  std::optional<uint32_t> offset;
  if (remotePosition.hasVisibleTextOffset && remotePosition.spineIndex == spineIndex) {
    offset = remotePosition.visibleTextOffset;
  }
  if (!EpubReaderUtils::saveProgress(*epub, spineIndex, page, 0, offset)) {
    if (mode == Mode::AUTOMATIC) {
      LOG_ERR("KOSync", "Automatic remote progress could not be saved");
      completeFlow("apply-save-failed");
      return;
    }
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  returnToReader();
}

void KOReaderSyncActivity::returnToReader() { completeFlow(); }

void KOReaderSyncActivity::completeFlow(const char* reason) {
  if (mode == Mode::AUTOMATIC) {
    LOG_DBG("KOSync", "Auto terminal trigger=%u reason=%s target=%u", static_cast<unsigned>(trigger), reason,
            static_cast<unsigned>(completionTarget));
  }
  if (mode == Mode::MANUAL || completionTarget == CompletionTarget::READER) {
    // The just-finished exchange must not immediately trigger a second open
    // exchange when the reader is recreated.
    activityManager.goToReader(epubPath, false, KOReaderSyncSession::suppressImmediateOpen(trigger));
  } else if (completionTarget == CompletionTarget::HOME) {
    activityManager.goHome();
  } else if (completionTarget == CompletionTarget::BOOK_SWITCH) {
    LOG_DBG("KOSync", "Auto continuation trigger=%u action=book-switch-open", static_cast<unsigned>(trigger));
    activityManager.goToReader(std::move(continuationPath), false, false, KOReaderSyncTrigger::Open);
  } else {
    completeDeferredDeepSleep(trigger == KOReaderSyncTrigger::Sleep);
  }
}

bool KOReaderSyncActivity::smartSyncEnabled() const {
  return KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
}

void KOReaderSyncActivity::markAutoReturn() { autoReturnAt = millis() + AUTO_RETURN_DELAY_MS; }

void KOReaderSyncActivity::completeAlreadySynced() {
  {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    completeFlow("wifi-failed");
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  // Keep the station fully awake for the short sync transaction. The web server
  // does the same because ESP32 modem sleep can introduce multi-second network
  // stalls that surface as HTTP timeouts. WiFi is torn down when this activity exits.
  WiFi.setSleep(false);
  LOG_DBG("KOSync", "WiFi sleep disabled for sync");

  if (mode == Mode::AUTOMATIC) {
    performAutomaticSync();
    return;
  }

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  // KOSync requests from CrossPoint do not include a client timestamp.
  performSync();
}

KOReaderProgress KOReaderSyncActivity::buildLocalProgress() const {
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;
  if (KOREADER_STORE.usesCrossPointSyncServer()) {
    KOReaderRichPosition position;
    const float pct = std::clamp(localProgress.percentage, 0.0f, 1.0f);
    position.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    position.spineIndex = static_cast<uint16_t>(std::max(0, currentSpineIndex));
    position.pageNumber = static_cast<uint16_t>(std::max(0, currentPage));
    position.totalPages = static_cast<uint16_t>(std::max(1, totalPagesInSpine));
    position.paragraphIndex = currentParagraphIndex;
    position.xpath = localProgress.xpath;
    progress.position = std::move(position);
  }
  progress.metadata = localMetadata;
  return progress;
}

void KOReaderSyncActivity::performAutomaticSync() {
  documentHash = calculateDocumentHashForMethod(epubPath, KOREADER_STORE.getMatchMethod());
  if (documentHash.empty()) {
    completeFlow("hash-failed");
    return;
  }

  KOReaderSyncSnapshot snapshot;
  snapshot.documentHash = documentHash;
  snapshot.localProgress = buildLocalProgress();
  const auto behavior =
      KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART ? KOReaderSyncMode::Smart : KOReaderSyncMode::Ask;
  if (!automaticClient) automaticClient = makeUniqueNoThrow<KOReaderSyncHttpSession>();
  if (!automaticClient) {
    LOG_DBG("KOSync", "Auto terminal trigger=%u action=continue result=%d", static_cast<unsigned>(trigger),
            static_cast<int>(KOReaderSyncClient::LOW_MEMORY));
    completeFlow("low-memory");
    return;
  }
  KOReaderSyncSession session(std::move(snapshot), trigger, behavior, automaticStartedAt, *automaticClient);
  KOReaderSyncOutcome outcome = session.run(millis());
  if (outcome.error != KOReaderSyncClient::OK || !outcome.hasRemoteProgress) {
    handleAutomaticOutcome(std::move(outcome));
    return;
  }

  remoteProgress = outcome.remoteProgress;
  if (!mapAutomaticRemoteProgress()) {
    completeFlow("mapping-unavailable");
    return;
  }

  const KOReaderSyncComparison comparison = compareAutomaticPositions();
  const KOReaderSyncPendingTransition next = KOReaderSyncSession::decide(trigger, behavior, comparison);
  if (next == KOReaderSyncPendingTransition::UploadLocal) {
    // Mapping is complete; release the book before the TLS write just as the
    // original reader-to-sync hand-off did.
    epub.reset();
  }

  outcome = session.resolve(comparison, millis());
  outcome.hasRemoteProgress = true;
  outcome.remoteProgress = remoteProgress;
  handleAutomaticOutcome(std::move(outcome));
}

bool KOReaderSyncActivity::mapAutomaticRemoteProgress() {
  ensureEpubLoaded();
  if (!epub) return false;

  const SavedProgressPosition remoteKoPos = {remoteProgress.progress, remoteProgress.percentage};
  remotePosition = ProgressMapper::toCrossPoint(epub, remoteKoPos, renderer, currentSpineIndex, totalPagesInSpine);
  if (!remotePosition.hasVisibleTextOffset && remoteProgress.position.has_value()) {
    const bool sameXPath = remoteProgress.position->xpath == remoteProgress.progress;
    if (const auto richMapped = ProgressMapper::fromRichPosition(epub, *remoteProgress.position, renderer, sameXPath)) {
      remotePosition = *richMapped;
    }
  }
  remotePositionCanBeApplied = remotePosition.hasResolvedSpineIndex &&
                               (remotePosition.hasVisibleTextOffset || remotePosition.hasReliableMappedPage);
  return true;
}

KOReaderSyncComparison KOReaderSyncActivity::compareAutomaticPositions() const {
  if (!localPosition.hasResolvedSpineIndex || !remotePosition.hasResolvedSpineIndex) {
    return KOReaderSyncComparison::Unknown;
  }
  if (localPosition.spineIndex != remotePosition.spineIndex) {
    return localPosition.spineIndex > remotePosition.spineIndex ? KOReaderSyncComparison::LocalAhead
                                                                : KOReaderSyncComparison::RemoteAhead;
  }
  // A shared reliable page is a stronger equality proof than different
  // renderer text-offset representations for the same page.
  if (localPosition.hasReliableMappedPage && remotePosition.hasReliableMappedPage &&
      localPosition.pageNumber == remotePosition.pageNumber) {
    return KOReaderSyncComparison::Equal;
  }
  if (localPosition.hasVisibleTextOffset && remotePosition.hasVisibleTextOffset) {
    if (localPosition.visibleTextOffset == remotePosition.visibleTextOffset) return KOReaderSyncComparison::Equal;
    return localPosition.visibleTextOffset > remotePosition.visibleTextOffset ? KOReaderSyncComparison::LocalAhead
                                                                              : KOReaderSyncComparison::RemoteAhead;
  }
  if (localPosition.hasReliableMappedPage && remotePosition.hasReliableMappedPage) {
    return localPosition.pageNumber > remotePosition.pageNumber ? KOReaderSyncComparison::LocalAhead
                                                                : KOReaderSyncComparison::RemoteAhead;
  }
  return KOReaderSyncComparison::Unknown;
}

void KOReaderSyncActivity::beginAutomaticTransaction() {
  automaticStartedAt = millis();
  WiFi.setSleep(false);
}

void KOReaderSyncActivity::applyAutomaticLatestRemote() {
  if (!automaticClient) {
    completeFlow("low-memory");
    return;
  }

  KOReaderProgress latestProgress;
  const uint32_t remaining = KOReaderSyncSession::remainingMs(millis(), automaticStartedAt);
  if (remaining == 0) {
    completeFlow("deadline-expired");
    return;
  }
  if (automaticClient->getProgress(documentHash, latestProgress, remaining) != KOReaderSyncClient::OK) {
    // A selected apply has no valid target if the record was deleted or the
    // revalidation request failed while the resolver was visible.
    completeFlow("apply-revalidation-failed");
    return;
  }

  remoteProgress = std::move(latestProgress);
  if (!mapAutomaticRemoteProgress() || !remotePositionCanBeApplied) {
    completeFlow("remote-unresolved");
    return;
  }
  saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
}

void KOReaderSyncActivity::handleAutomaticOutcome(KOReaderSyncOutcome outcome) {
  LOG_DBG("KOSync", "Auto outcome trigger=%u action=%u result=%d", static_cast<unsigned>(trigger),
          static_cast<unsigned>(outcome.pending), static_cast<int>(outcome.error));
  if (outcome.pending == KOReaderSyncPendingTransition::ContinueSleep) {
    completeFlow("sleep-continued");
    return;
  }
  if (outcome.error != KOReaderSyncClient::OK) {
    completeFlow("session-error");
    return;
  }
  if (outcome.pending == KOReaderSyncPendingTransition::None) {
    completeFlow("session-complete");
    return;
  }

  hasRemoteProgress = outcome.hasRemoteProgress;
  remoteProgress = std::move(outcome.remoteProgress);
  if (!hasRemoteProgress) {
    WiFi.setSleep(true);
    state = NO_REMOTE_PROGRESS;
    requestUpdate(true);
    return;
  }

  if (outcome.pending == KOReaderSyncPendingTransition::ApplyRemote) {
    if (!remotePositionCanBeApplied) {
      completeFlow("remote-unresolved");
      return;
    }
    saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
    return;
  }

  WiFi.setSleep(true);
  selectedOption = (!remotePositionCanBeApplied || outcome.comparison == KOReaderSyncComparison::LocalAhead) ? 1 : 0;
  state = SHOWING_RESULT;
  requestUpdate(true);
}

void KOReaderSyncActivity::performSync() {
  const DocumentMatchMethod primaryMethod = KOREADER_STORE.getMatchMethod();
  documentHash = calculateDocumentHashForMethod(epubPath, primaryMethod);
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }
  const std::string primaryHash = documentHash;

  LOG_DBG("KOSync", "Document hash calculated using %s matching", matchMethodName(primaryMethod));

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  // Fetch remote progress. In smart mode, also probe the alternate document-id
  // method and use the furthest remote state we can find. This avoids a stale
  // local upload when another KOReader device synced the same book with a
  // different document matching method.
  auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);
  LOG_DBG("KOSync", "Primary remote (%s): result=%d http=%d local=%.6f remote=%.6f", matchMethodName(primaryMethod),
          result, KOReaderSyncClient::lastHttpCode, localProgress.percentage, remoteProgress.percentage);

  if (smartSyncEnabled()) {
    const DocumentMatchMethod altMethod = alternateMatchMethod(primaryMethod);
    const std::string altHash = calculateDocumentHashForMethod(epubPath, altMethod);
    if (!altHash.empty() && altHash != documentHash) {
      KOReaderProgress altProgress;
      const auto altResult = KOReaderSyncClient::getProgress(altHash, altProgress);
      LOG_DBG("KOSync", "Alternate remote (%s): result=%d http=%d local=%.6f remote=%.6f", matchMethodName(altMethod),
              altResult, KOReaderSyncClient::lastHttpCode, localProgress.percentage, altProgress.percentage);

      if (altResult == KOReaderSyncClient::OK &&
          (result == KOReaderSyncClient::NOT_FOUND || altProgress.percentage > remoteProgress.percentage)) {
        documentHash = altHash;
        remoteProgress = std::move(altProgress);
        result = KOReaderSyncClient::OK;
      }
    }
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (smartSyncEnabled()) {
      LOG_DBG("KOSync", "Smart sync: no remote progress found; uploading local %.6f", localProgress.percentage);
      performUpload();
      return;
    }

    // No remote progress - offer to upload
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  // Epub was released before sync to free RAM for the TLS handshake — reload it now.
  hasRemoteProgress = true;
  ensureEpubLoaded();
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  // The standard KOReader progress XPath is the authoritative content anchor.
  // The CrossPoint server's existing rich page hints remain a legacy fallback.
  SavedProgressPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
  remotePosition = ProgressMapper::toCrossPoint(epub, koPos, renderer, currentSpineIndex, totalPagesInSpine);
  if (!remotePosition.hasVisibleTextOffset && remoteProgress.position.has_value()) {
    // toCrossPoint above already tried koPos.xpath; if the rich position carries the same XPath,
    // tell fromRichPosition to skip re-resolving it and use its page hints directly.
    const bool sameXPath = remoteProgress.position->xpath == remoteProgress.progress;
    if (const auto richMapped = ProgressMapper::fromRichPosition(epub, *remoteProgress.position, renderer, sameXPath)) {
      remotePosition = *richMapped;
    }
  }

  if (smartSyncEnabled()) {
    static constexpr float SAME_PROGRESS_EPSILON = 0.001f;  // 0.1 percentage points
    const float delta = localProgress.percentage - remoteProgress.percentage;
    LOG_DBG("KOSync", "Smart decision: local=%.6f remote=%.6f delta=%.6f mapped=%d/%d", localProgress.percentage,
            remoteProgress.percentage, delta, remotePosition.spineIndex, remotePosition.pageNumber);
    if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) {
      completeAlreadySynced();
      return;
    }

    if (delta > 0) {
      // Alternate hashes are only probes for newer remote state. Keep uploads
      // on the user's configured matching method so its primary record heals.
      documentHash = primaryHash;
      performUpload();
      return;
    }

    saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
    return;
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;

    // Default to the option that corresponds to the furthest progress
    if (localProgress.percentage > remoteProgress.percentage) {
      selectedOption = 1;  // Upload local progress
    } else {
      selectedOption = 0;  // Apply remote progress
    }
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  KOReaderProgress progress = buildLocalProgress();

  // Metadata and the rich position were frozen before reader resources were
  // released, so this write does not need an EPUB resident during TLS.
  epub.reset();

  // An Ask resolver can remain on screen while the server changes. Automatic
  // uploads therefore revalidate the configured document with a fresh GET
  // immediately before every PUT. The session's absolute deadline also makes
  // a stale resolver fail closed instead of writing after its transaction.
  if (mode == Mode::AUTOMATIC) {
    WiFi.setSleep(false);
    if (!automaticClient) {
      completeFlow("low-memory");
      return;
    }
    KOReaderProgress latestProgress;
    const uint32_t remaining = KOReaderSyncSession::remainingMs(millis(), automaticStartedAt);
    if (remaining == 0) {
      completeFlow("deadline-expired");
      return;
    }
    const auto latestResult = automaticClient->getProgress(documentHash, latestProgress, remaining);
    if (latestResult != KOReaderSyncClient::OK && latestResult != KOReaderSyncClient::NOT_FOUND) {
      completeFlow("revalidation-failed");
      return;
    }
  }

  const uint32_t remaining = mode == Mode::AUTOMATIC ? KOReaderSyncSession::remainingMs(millis(), automaticStartedAt)
                                                     : KOReaderSyncSession::DEADLINE_MS;
  if (remaining == 0) {
    completeFlow("deadline-expired");
    return;
  }
  const auto result = mode == Mode::AUTOMATIC ? automaticClient->updateProgress(progress, remaining)
                                              : KOReaderSyncClient::updateProgress(progress, remaining);

  // Manual sync retains the historic restart cleanup. Automatic sync only
  // releases a station connection it created, in onExit().
  if (mode == Mode::MANUAL) esp_wifi_stop();

  if (result != KOReaderSyncClient::OK) {
    if (mode == Mode::AUTOMATIC) {
      completeFlow("upload-failed");
      return;
    }
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  if (mode == Mode::AUTOMATIC) {
    completeFlow("upload-complete");
    return;
  }

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  resetUi();
  app.on(ACTION_ROW, &KOReaderSyncActivity::onResultRow, this);
  app.setScreen(&KOReaderSyncActivity::resultScreen, this);

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    if (mode == Mode::AUTOMATIC) {
      completeFlow();
      return;
    }
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  wifiWasAlreadyConnected = WiFi.status() == WL_CONNECTED;
  wifiActivated = mode == Mode::AUTOMATIC ? !wifiWasAlreadyConnected : true;

  // Check if already connected (e.g. from settings page auth)
  if (wifiWasAlreadyConnected) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection subactivity
  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  const auto wifiMode = mode == Mode::AUTOMATIC ? WifiAutoConnectMode::Background : WifiAutoConnectMode::Interactive;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(
                             renderer, mappedInput, true, wifiMode,
                             mode == Mode::AUTOMATIC ? std::optional<uint32_t>(automaticStartedAt) : std::nullopt),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  if (mode == Mode::AUTOMATIC) {
    if (KOReaderSyncSession::shouldDisconnectAutomaticWifi(wifiWasAlreadyConnected, wifiActivated)) {
      WiFi.disconnect(false);
    }
    WiFi.setSleep(true);
    return;
  }

  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

void KOReaderSyncActivity::chooseResultOption() {
  if (mode == Mode::AUTOMATIC) {
    beginAutomaticTransaction();
    if (selectedOption == 0) {
      applyAutomaticLatestRemote();
    } else {
      performUpload();
    }
    return;
  }
  if (selectedOption == 0) {
    saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
  } else {
    performUpload();
  }
}

void KOReaderSyncActivity::startUpload() {
  if (documentHash.empty()) {
    documentHash = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                       ? KOReaderDocumentId::calculateFromFilename(epubPath)
                       : KOReaderDocumentId::calculate(epubPath);
  }
  if (mode == Mode::AUTOMATIC) beginAutomaticTransaction();
  performUpload();
}

void KOReaderSyncActivity::onResultRow(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<KOReaderSyncActivity*>(user);
  // Activation leaves this screen (applies/uploads); drop the flash so it does
  // not ghost onto the next paint.
  self->app.clearTapFlash();
  if (self->state == SHOWING_RESULT) {
    if (event.value < 0 || event.value > 1) return;
    self->selectedOption = event.value;
    self->chooseResultOption();
  } else if (self->state == NO_REMOTE_PROGRESS) {
    self->startUpload();
  }
}

void KOReaderSyncActivity::resultScreen(UiScreen& screen, void* user) {
  static_cast<KOReaderSyncActivity*>(user)->buildResultScreen(screen);
}

void KOReaderSyncActivity::buildResultScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Side padding is 0 here (like the other FreeInkApp screens): the action list
  // supplies its own theme side padding, and the raw comparison text is indented
  // to line up with the list rows below (see labelIndent).
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (state == SHOWING_RESULT) {
    // Chapter names (remote requires the lazily-loaded Epub; local was
    // pre-computed before the Epub was released).
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    char remoteVal[64];
    snprintf(remoteVal, sizeof(remoteVal), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    char localVal[64];
    snprintf(localVal, sizeof(localVal), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    char deviceStr[80];
    deviceStr[0] = '\0';
    if (!remoteProgress.device.empty()) {
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
    }

    // Labeled, multi-line comparison flowing from the top. Indent everything to
    // the list rows' content-left (the row inset + side padding the list adds
    // below) so the "Remote"/"Local" labels sit directly above the row icons.
    auto labelStyle = screen.theme().bodyText;
    labelStyle.bold = true;
    auto detailStyle = screen.theme().smallText;
    const int16_t labelH = screen.target().lineHeight(labelStyle.font);
    const int16_t detailH = screen.target().lineHeight(detailStyle.font);
    const int16_t labelIndent = static_cast<int16_t>(screen.theme().listInset + screen.theme().listSidePadding);
    const int16_t detailIndent = static_cast<int16_t>(labelIndent + screen.theme().spaceMd);
    const auto textLine = [&](const char* text, const fui::TextStyle& style, int16_t height, int16_t indent,
                              int16_t gap) {
      fui::Rect r = screen.takeTop(height, gap);
      r.x = static_cast<int16_t>(r.x + indent);
      r.width = static_cast<int16_t>(r.width - indent);
      screen.target().text(r, text, style);
    };
    const auto labelLine = [&](const char* text) {
      textLine(text, labelStyle, labelH, labelIndent, screen.theme().spaceSm);
    };
    const auto detailLine = [&](const char* text) {
      textLine(text, detailStyle, detailH, detailIndent, screen.theme().spaceXs);
    };

    labelLine(tr(STR_REMOTE_LABEL));
    detailLine(remoteChapter.c_str());
    detailLine(remoteVal);
    if (deviceStr[0] != '\0') detailLine(deviceStr);
    screen.spacer(screen.theme().spaceLg);
    labelLine(tr(STR_LOCAL_LABEL));
    detailLine(localChapter.c_str());
    detailLine(localVal);

    // Two themed action rows flowing directly below the labels (not anchored to
    // the bottom). Rendered through the list component so they inherit the
    // active theme's row radius, insets, and selection style, matching every
    // other selectable list in the UI. Apply Remote pulls (download), Upload
    // Local pushes (upload); the selected row highlights for physical-button
    // users and tap works either way.
    screen.spacer(screen.theme().spaceMd);
    fui::ListItem actions[2];
    actions[0].label = tr(STR_APPLY_REMOTE);
    actions[0].icon = fui::bitmapFromIcon(icon_download_24);
    actions[0].actionValue = 0;
    actions[1].label = tr(STR_UPLOAD_LOCAL);
    actions[1].icon = fui::bitmapFromIcon(icon_upload_24);
    actions[1].actionValue = 1;
    fui::ListProps actionProps;
    actionProps.items = actions;
    actionProps.count = 2;
    actionProps.selectedIndex = static_cast<int16_t>(selectedOption);
    actionProps.action = ACTION_ROW;
    actionProps.inputMask = fui::InputTouch;  // physical buttons stay in loop()
    actionProps.scrollIndicator = false;      // never scrolls; no indicator needed
    // Non-touch hardware (X3/X4) keeps the original, denser row height instead
    // of FreeInkUI's touch-target-sized default (see
    // UiListActivity::syncListViewport); actionsBand must use the same value
    // or the band and the rows it contains fall out of sync.
    int16_t actionRowHeight = screen.theme().rowHeight;
    if (!mappedInput.hasTouch()) {
      actionRowHeight = static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight);
      actionProps.rowHeight = actionRowHeight;
    }
    // Keep the theme's row inset + side padding so the selected-row highlight has
    // the same padding around its icon/label as every other list in the UI; the
    // labels above are indented to match this content-left.
    const auto actionsBand =
        static_cast<int16_t>(actionRowHeight * 2 + screen.theme().listRowGap + screen.theme().spaceSm);
    screen.list(actionProps, actionsBand);
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    auto centered = screen.theme().bodyText;
    centered.align = fui::TextAlign::Center;
    auto centeredBold = centered;
    centeredBold.bold = true;
    const int16_t lineH = screen.target().lineHeight(centered.font);
    screen.target().text(screen.takeTop(lineH, screen.theme().spaceSm), tr(STR_NO_REMOTE_MSG), centeredBold);
    screen.target().text(screen.takeTop(lineH, screen.theme().spaceMd), tr(STR_UPLOAD_PROMPT), centered);

    // Single themed action row anchored to the bottom, matching the lists used
    // everywhere else (inherits the theme's row radius + selection style).
    fui::ListItem action;
    action.label = tr(STR_UPLOAD_LOCAL);
    action.actionValue = 0;
    fui::ListProps actionProps;
    actionProps.items = &action;
    actionProps.count = 1;
    actionProps.selectedIndex = 0;
    actionProps.action = ACTION_ROW;
    actionProps.inputMask = fui::InputTouch;
    actionProps.scrollIndicator = false;
    // See the equivalent override above; keeps actionsBand in sync with the
    // row height actually used on non-touch hardware (X3/X4).
    int16_t actionRowHeight = screen.theme().rowHeight;
    if (!mappedInput.hasTouch()) {
      actionRowHeight = static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight);
      actionProps.rowHeight = actionRowHeight;
    }
    const auto actionsBand = static_cast<int16_t>(actionRowHeight + screen.theme().spaceMd);
    screen.list(actionProps, actionsBand, fui::LayoutAnchor::Bottom);
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 state == SHOWING_RESULT ? tr(STR_PROGRESS_FOUND) : tr(STR_KOREADER_SYNC));

  int top = screen.y + screen.height / 2 - 40;
  if (state == NO_CREDENTIALS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_KOREADER_SETUP_HINT), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    // Comparison rows + option selection render through the FreeInkApp
    // (themed rows, tap-flash); the header above shows "Progress Found".
    renderUi();

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    // Prompt text + upload button render through the FreeInkApp.
    renderUi();

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top,
                              state == UPLOAD_COMPLETE ? tr(STR_UPLOAD_SUCCESS) : tr(STR_ALREADY_SYNCED), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, statusMessage.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      returnToReader();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    // Touch goes through the FreeInkApp: render() registered the compare rows;
    // route the snapshot and let onResultRow apply/upload on tap.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onResultRow

    // Navigate the two options with physical buttons.
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      chooseResultOption();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    // Touch goes through the FreeInkApp: render() registered the upload button.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onResultRow -> startUpload

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }
}
