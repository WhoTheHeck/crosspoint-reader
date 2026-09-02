#include "ChapterXPathResolver.h"

std::string ChapterXPathResolver::findXPathForParagraph(const std::shared_ptr<Epub>&, int, uint16_t) { return {}; }

std::string ChapterXPathResolver::findXPathForProgress(const std::shared_ptr<Epub>&, int, float) { return {}; }

std::string ChapterXPathResolver::findXPathForVisibleTextOffset(const std::shared_ptr<Epub>&, int, uint32_t) {
  return {};
}
