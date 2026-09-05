#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "EndOfBookOptions.h"
#include "KOReaderSyncSession.h"
#include "activities/Activity.h"

class ReaderActivity : public Activity {
 protected:
  std::string bookPath;
  // Completion can suppress exactly the replacement reader's first automatic
  // open check. Home, sleep, and later book-switch checks stay eligible.
  bool suppressAutomaticOpenOnce = false;
  int pagesUntilFullRefresh = 0;
  bool forcedRefreshPending = false;

  std::unique_ptr<EndOfBookOptions> endOfBookOptions;
  std::atomic<bool> endOfBookOptionsReady{false};

  explicit ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                          std::string bookPath, bool allowFastInitialRefresh, bool suppressAutomaticOpenOnce,
                          KOReaderSyncTrigger automaticTrigger);
  KOReaderSyncTrigger automaticTrigger = KOReaderSyncTrigger::Open;

  virtual bool loadBook() = 0;
  virtual std::string getBookTitle() const = 0;
  virtual std::string getBookAuthor() const { return ""; }
  virtual std::string getBookThumbBmpPath() const { return ""; }

  virtual bool handleFormatInput() { return false; }
  virtual bool pageTurn(bool isForward) = 0;
  virtual bool skipPages(int amount) { return pageTurn(amount > 0); }
  virtual bool isAtEndOfBook() const = 0;
  virtual void onReturnFromEndOfBook() {}

  virtual void renderBook() = 0;
  virtual void applyInitialOrientation();
  virtual void onEndOfBookRendered() {}

  bool handleBackNavigation();
  /** True while the end-of-book suggestion menu is on screen and owning input. */
  bool endOfBookMenuActive() const;
  bool handleEndOfBookMenu(bool suppressConfirmRelease = false);
  bool handleEndOfBookPageTurn(bool prevTriggered, bool nextTriggered);
  void clearEndOfBookOptionsIfNeeded();
  void disableFastInitialRefresh();

 public:
  ~ReaderActivity() override = default;

  static std::unique_ptr<ReaderActivity> create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                std::string path, bool allowFastInitialRefresh,
                                                bool suppressAutomaticOpenOnce = false,
                                                KOReaderSyncTrigger automaticTrigger = KOReaderSyncTrigger::Open);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

  bool isReaderActivity() const final { return true; }
  bool handleForcedRefresh() final;
};
