#include "CssParser.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string_view>

namespace {

// Stack-allocated string buffer to avoid heap reallocations during parsing
// Provides string-like interface with fixed capacity
struct StackBuffer {
  static constexpr size_t CAPACITY = 1024;
  char data[CAPACITY];
  size_t len = 0;

  void push_back(char c) {
    if (len < CAPACITY - 1) {
      data[len++] = c;
    }
  }

  void clear() { len = 0; }
  bool empty() const { return len == 0; }
  size_t size() const { return len; }

  // Get string view of current content (zero-copy)
  std::string_view view() const { return std::string_view(data, len); }
  operator std::string_view() const noexcept { return view(); }
};

// Buffer size for reading CSS files
constexpr size_t READ_BUFFER_SIZE = 512;

// Maximum number of CSS rules to store in the selector map
// Prevents unbounded memory growth from pathological CSS files
constexpr size_t MAX_RULES = 1500;

// Minimum free heap required to apply CSS during rendering
// If below this threshold, we skip CSS to avoid display artifacts.
constexpr size_t MIN_FREE_HEAP_FOR_CSS = 48 * 1024;

// Maximum length for a single selector string
// Prevents parsing of extremely long or malformed selectors
constexpr size_t MAX_SELECTOR_LENGTH = 256;

constexpr std::string_view BACKGROUND_IMAGE_FUNCTION = "url";

// Check if character is CSS whitespace
constexpr bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

constexpr std::string_view trimCssWhitespace(std::string_view s) {
  while (!s.empty() && isCssWhitespace(s.front())) s.remove_prefix(1);
  while (!s.empty() && isCssWhitespace(s.back())) s.remove_suffix(1);
  return s;
}

constexpr char asciiToLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// Case-insensitive equality on ASCII. lowercaseKeyword MUST already be
// lowercase; CSS keywords are ASCII by spec so byte-wise tolower is safe.
constexpr bool iequalsAscii(std::string_view value, std::string_view lowercaseKeyword) {
  return std::equal(value.begin(), value.end(), lowercaseKeyword.begin(), lowercaseKeyword.end(),
                    [](char a, char b) { return asciiToLower(a) == b; });
}

// Walk s and invoke fn(token) for each non-empty run between delimiters.
// Tokens are boundary-trimmed and yielded as string_views into s; no
// allocation. Runs of consecutive delimiters coalesce — no empty tokens are
// emitted. `isDelimiter` is invoked once per character.
template <typename Pred, typename F>
void forEachDelimitedToken(std::string_view s, Pred isDelimiter, F&& fn) {
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || isDelimiter(s[i])) {
      const std::string_view trimmed = trimCssWhitespace(s.substr(start, i - start));
      if (!trimmed.empty()) {
        fn(trimmed);
      }
      start = i + 1;
    }
  }
}

// FNV-1a per Fowler/Noll/Vo, sized to match size_t on the target. The firmware
// runs on a 32-bit core where size_t is 32 bits, so naively using the 64-bit
// constants would silently truncate FNV_PRIME to a non-prime and wreck hash
// distribution. The selection below picks the canonical 32- or 64-bit
// constants at compile time so the same source works in a 64-bit host
// simulator. `fnv1aMix` is the per-byte mix step; callers apply any
// byte-level transform (e.g. asciiToLower) first.
static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8, "FNV constants are only defined for 32- or 64-bit size_t");
constexpr size_t FNV_OFFSET_BASIS =
    sizeof(size_t) == 8 ? static_cast<size_t>(14695981039346656037ULL) : static_cast<size_t>(2166136261U);
constexpr size_t FNV_PRIME =
    sizeof(size_t) == 8 ? static_cast<size_t>(1099511628211ULL) : static_cast<size_t>(16777619U);

constexpr size_t fnv1aMix(size_t hash, unsigned char byte) { return (hash ^ byte) * FNV_PRIME; }

// Parse the entirety of s as a number into `out`. Accepts an optional leading
// '+' (which std::from_chars rejects by spec) so callers can pass CSS-style
// signed numbers without manual trimming. Returns false on empty input, a
// non-numeric suffix, or any from_chars error.
template <typename T>
bool tryParseNumber(std::string_view s, T& out) {
  const char* begin = s.data();
  const char* end = s.data() + s.size();
  if (begin < end && *begin == '+') ++begin;
  const auto r = std::from_chars(begin, end, out);
  return r.ec == std::errc{} && r.ptr == end;
}

// Collect up to 4 whitespace-separated tokens for a CSS edge-value shorthand
// (margin, padding, and the border-* family). Returns the number of tokens
// written; extras are silently dropped. Callers apply the 1/2/3/4-value
// fallback rule using the returned count.
size_t collectEdgeValueTokens(std::string_view s, std::string_view (&out)[4]) {
  size_t count = 0;
  forEachDelimitedToken(s, isCssWhitespace, [&](std::string_view tok) {
    if (count < 4) out[count++] = tok;
  });
  return count;
}

std::string_view stripTrailingImportant(std::string_view value) {
  constexpr std::string_view IMPORTANT = "!important";

  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }

  if (value.size() < IMPORTANT.size()) {
    return value;
  }

  const size_t suffixPos = value.size() - IMPORTANT.size();
  if (!iequalsAscii(value.substr(suffixPos), IMPORTANT)) {
    return value;
  }

  value.remove_suffix(IMPORTANT.size());
  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

bool hasAsciiPrefixInsensitive(const std::string_view value, const std::string_view prefix) {
  return value.size() >= prefix.size() && iequalsAscii(value.substr(0, prefix.size()), prefix);
}

bool isLocalBackgroundPath(const std::string_view path) {
  if (path.empty() || path.front() == '/' || path.find("//") != std::string_view::npos ||
      path.find('\\') != std::string_view::npos) {
    return false;
  }
  if (hasAsciiPrefixInsensitive(path, "data:") || hasAsciiPrefixInsensitive(path, "http:") ||
      hasAsciiPrefixInsensitive(path, "https:") || hasAsciiPrefixInsensitive(path, "file:") ||
      hasAsciiPrefixInsensitive(path, "javascript:")) {
    return false;
  }
  // Query strings and fragments are intentionally excluded: EPUB item names
  // are archive paths, and retaining either suffix would defeat extension and
  // cache-path validation below.
  return path.find_first_of("?#") == std::string_view::npos;
}

bool staysWithinArchiveRoot(const std::string_view directory, const std::string_view path) {
  size_t depth = 0;
  bool escaped = false;
  const auto consume = [&depth, &escaped](const std::string_view value) {
    size_t start = 0;
    for (size_t i = 0; i <= value.size(); ++i) {
      if (i != value.size() && value[i] != '/') continue;
      const std::string_view component = value.substr(start, i - start);
      if (component == "..") {
        if (depth == 0) {
          escaped = true;
        } else {
          --depth;
        }
      } else if (!component.empty() && component != ".") {
        ++depth;
      }
      start = i + 1;
    }
  };
  consume(directory);
  consume(path);
  return !escaped;
}

}  // anonymous namespace

// Transparent case-insensitive hash/equal. Bodies live here (rather than
// inline in the header) so they can share the anonymous-namespace asciiToLower
// with the other ASCII helpers in this translation unit.

size_t CssParser::SvHash::operator()(std::string_view sv) const noexcept {
  size_t h = FNV_OFFSET_BASIS;
  for (char c : sv) h = fnv1aMix(h, asciiToLower(c));
  return h;
}

size_t CssParser::SvHash::operator()(const std::string& s) const noexcept { return operator()(std::string_view(s)); }

size_t CssParser::SvHash::operator()(CompositeKey k) const noexcept {
  // Hash the case-folded concatenation of every piece without materializing
  // it — the running hash continues across pieces as if they were one buffer.
  size_t h = FNV_OFFSET_BASIS;
  for (std::string_view piece : k.pieces) {
    for (char c : piece) h = fnv1aMix(h, asciiToLower(c));
  }
  return h;
}

bool CssParser::SvEqual::operator()(std::string_view a, std::string_view b) const noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (asciiToLower(a[i]) != asciiToLower(b[i])) return false;
  }
  return true;
}

bool CssParser::SvEqual::operator()(const std::string& a, std::string_view b) const noexcept {
  return operator()(std::string_view(a), b);
}

bool CssParser::SvEqual::operator()(std::string_view a, const std::string& b) const noexcept {
  return operator()(a, std::string_view(b));
}

bool CssParser::SvEqual::operator()(const std::string& a, const std::string& b) const noexcept {
  return operator()(std::string_view(a), std::string_view(b));
}

bool CssParser::SvEqual::operator()(CompositeKey k, std::string_view sv) const noexcept {
  size_t total = 0;
  for (std::string_view piece : k.pieces) total += piece.size();
  if (total != sv.size()) return false;
  size_t i = 0;
  for (std::string_view piece : k.pieces) {
    for (char c : piece) {
      if (asciiToLower(c) != asciiToLower(sv[i++])) return false;
    }
  }
  return true;
}

bool CssParser::SvEqual::operator()(std::string_view sv, CompositeKey k) const noexcept { return operator()(k, sv); }

// Property value interpreters

CssTextAlign CssParser::interpretAlignment(std::string_view val) {
  val = trimCssWhitespace(val);

  if (iequalsAscii(val, "left") || iequalsAscii(val, "start")) return CssTextAlign::Left;
  if (iequalsAscii(val, "right") || iequalsAscii(val, "end")) return CssTextAlign::Right;
  if (iequalsAscii(val, "center")) return CssTextAlign::Center;
  if (iequalsAscii(val, "justify")) return CssTextAlign::Justify;

  return CssTextAlign::Left;
}

CssFontStyle CssParser::interpretFontStyle(std::string_view val) {
  val = trimCssWhitespace(val);

  if (iequalsAscii(val, "italic") || iequalsAscii(val, "oblique")) return CssFontStyle::Italic;
  return CssFontStyle::Normal;
}

CssFontWeight CssParser::interpretFontWeight(std::string_view val) {
  val = trimCssWhitespace(val);

  // Named values
  if (iequalsAscii(val, "bold") || iequalsAscii(val, "bolder")) return CssFontWeight::Bold;
  if (iequalsAscii(val, "normal") || iequalsAscii(val, "lighter")) return CssFontWeight::Normal;

  // Numeric values: 100-900
  // CSS spec: 400 = normal, 700 = bold
  // We use: 0-400 = normal, 700+ = bold, 500-600 = normal (conservative)
  long numericWeight = 0;
  if (tryParseNumber(val, numericWeight)) {
    return numericWeight >= 700 ? CssFontWeight::Bold : CssFontWeight::Normal;
  }
  return CssFontWeight::Normal;
}

CssTextDecoration CssParser::interpretDecoration(std::string_view val) {
  // text-decoration can have multiple space-separated values. Compare whole tokens
  // so malformed values like "notunderline" do not accidentally enable a line.
  CssTextDecoration result = CssTextDecoration::None;
  bool explicitNone = false;
  forEachDelimitedToken(val, isCssWhitespace, [&](const std::string_view token) {
    if (iequalsAscii(token, "none")) {
      explicitNone = true;
    } else if (iequalsAscii(token, "underline")) {
      result = result | CssTextDecoration::Underline;
    } else if (iequalsAscii(token, "line-through")) {
      result = result | CssTextDecoration::LineThrough;
    }
  });
  return explicitNone ? CssTextDecoration::None : result;
}

CssLength CssParser::interpretLength(std::string_view val) {
  CssLength result;
  tryInterpretLength(val, result);
  return result;
}

bool CssParser::tryInterpretLength(std::string_view val, CssLength& out) {
  val = trimCssWhitespace(val);
  if (val.empty()) {
    out = CssLength{};
    return false;
  }

  size_t unitStart = val.size();
  for (size_t i = 0; i < val.size(); ++i) {
    const char c = val[i];
    if (!std::isdigit(c) && c != '.' && c != '-' && c != '+') {
      unitStart = i;
      break;
    }
  }

  float numericValue;
  if (!tryParseNumber(val.substr(0, unitStart), numericValue)) {
    out = CssLength{};
    return false;  // No number parsed (e.g. auto, inherit, initial)
  }

  const std::string_view unitPart = val.substr(unitStart);
  auto unit = CssUnit::Pixels;
  if (iequalsAscii(unitPart, "em")) {
    unit = CssUnit::Em;
  } else if (iequalsAscii(unitPart, "rem")) {
    unit = CssUnit::Rem;
  } else if (iequalsAscii(unitPart, "pt")) {
    unit = CssUnit::Points;
  } else if (unitPart == "%") {
    unit = CssUnit::Percent;
  }

  out = CssLength{numericValue, unit};
  return true;
}

// Declaration parsing

void CssParser::parseDeclarationIntoStyle(const std::string_view decl, CssStyle& style,
                                          const std::string_view stylesheetDirectory) {
  const size_t colonPos = decl.find(':');
  if (colonPos == std::string_view::npos || colonPos == 0) return;

  const std::string_view name = trimCssWhitespace(decl.substr(0, colonPos));
  std::string_view value = trimCssWhitespace(decl.substr(colonPos + 1));

  if (name.empty() || value.empty()) return;

  if (iequalsAscii(name, "text-align")) {
    style.textAlign = interpretAlignment(value);
    style.defined.textAlign = 1;
  } else if (iequalsAscii(name, "font-style")) {
    style.fontStyle = interpretFontStyle(value);
    style.defined.fontStyle = 1;
  } else if (iequalsAscii(name, "font-weight")) {
    style.fontWeight = interpretFontWeight(value);
    style.defined.fontWeight = 1;
  } else if (iequalsAscii(name, "text-decoration") || iequalsAscii(name, "text-decoration-line")) {
    style.textDecoration = interpretDecoration(value);
    style.defined.textDecoration = 1;
  } else if (iequalsAscii(name, "text-indent")) {
    style.textIndent = interpretLength(value);
    style.defined.textIndent = 1;
  } else if (iequalsAscii(name, "margin-top")) {
    style.marginTop = interpretLength(value);
    style.defined.marginTop = 1;
  } else if (iequalsAscii(name, "margin-bottom")) {
    style.marginBottom = interpretLength(value);
    style.defined.marginBottom = 1;
  } else if (iequalsAscii(name, "margin-left")) {
    style.marginLeft = interpretLength(value);
    style.defined.marginLeft = 1;
  } else if (iequalsAscii(name, "margin-right")) {
    style.marginRight = interpretLength(value);
    style.defined.marginRight = 1;
  } else if (iequalsAscii(name, "margin")) {
    std::string_view margins[4];
    const size_t count = collectEdgeValueTokens(value, margins);
    if (count > 0) {
      style.marginTop = interpretLength(margins[0]);
      style.marginRight = count >= 2 ? interpretLength(margins[1]) : style.marginTop;
      style.marginBottom = count >= 3 ? interpretLength(margins[2]) : style.marginTop;
      style.marginLeft = count >= 4 ? interpretLength(margins[3]) : style.marginRight;
      style.defined.marginTop = style.defined.marginRight = style.defined.marginBottom = style.defined.marginLeft = 1;
    }
  } else if (iequalsAscii(name, "padding-top")) {
    style.paddingTop = interpretLength(value);
    style.defined.paddingTop = 1;
  } else if (iequalsAscii(name, "padding-bottom")) {
    style.paddingBottom = interpretLength(value);
    style.defined.paddingBottom = 1;
  } else if (iequalsAscii(name, "padding-left")) {
    style.paddingLeft = interpretLength(value);
    style.defined.paddingLeft = 1;
  } else if (iequalsAscii(name, "padding-right")) {
    style.paddingRight = interpretLength(value);
    style.defined.paddingRight = 1;
  } else if (iequalsAscii(name, "padding")) {
    std::string_view paddings[4];
    const size_t count = collectEdgeValueTokens(value, paddings);
    if (count > 0) {
      style.paddingTop = interpretLength(paddings[0]);
      style.paddingRight = count >= 2 ? interpretLength(paddings[1]) : style.paddingTop;
      style.paddingBottom = count >= 3 ? interpretLength(paddings[2]) : style.paddingTop;
      style.paddingLeft = count >= 4 ? interpretLength(paddings[3]) : style.paddingRight;
      style.defined.paddingTop = style.defined.paddingRight = style.defined.paddingBottom = style.defined.paddingLeft =
          1;
    }
  } else if (iequalsAscii(name, "height")) {
    CssLength len;
    if (tryInterpretLength(value, len)) {
      style.imageHeight = len;
      style.defined.imageHeight = 1;
    }
  } else if (iequalsAscii(name, "width")) {
    CssLength len;
    if (tryInterpretLength(value, len)) {
      style.imageWidth = len;
      style.defined.imageWidth = 1;
    }
  } else if (iequalsAscii(name, "display")) {
    const std::string_view displayValue = stripTrailingImportant(value);
    style.display = iequalsAscii(displayValue, "none") ? CssDisplay::None : CssDisplay::Block;
    style.defined.display = 1;
  } else if (iequalsAscii(name, "direction")) {
    const std::string_view directionValue = stripTrailingImportant(value);
    if (iequalsAscii(directionValue, "rtl")) {
      style.direction = CssTextDirection::Rtl;
      style.defined.direction = 1;
    } else if (iequalsAscii(directionValue, "ltr")) {
      style.direction = CssTextDirection::Ltr;
      style.defined.direction = 1;
    }
  } else if (iequalsAscii(name, "vertical-align")) {
    if (iequalsAscii(value, "super")) {
      style.verticalAlign = CssVerticalAlign::Super;
      style.defined.verticalAlign = 1;
    } else if (iequalsAscii(value, "sub")) {
      style.verticalAlign = CssVerticalAlign::Sub;
      style.defined.verticalAlign = 1;
    }
  } else if (!stylesheetDirectory.empty() && iequalsAscii(name, "background-image")) {
    value = stripTrailingImportant(value);
    std::string rawPath;
    if (iequalsAscii(value, "none")) {
      style.backgroundImagePath = 0;
      style.defined.backgroundImage = 1;
    } else if (tryParseBackgroundImage(value, rawPath)) {
      const std::string decodedPath = FsHelpers::decodeUriEscapes(rawPath);
      const std::string resolvedPath = FsHelpers::normalisePath(std::string(stylesheetDirectory) + decodedPath);
      if (isLocalBackgroundPath(decodedPath) && staysWithinArchiveRoot(stylesheetDirectory, decodedPath) &&
          (FsHelpers::hasJpgExtension(resolvedPath) || FsHelpers::hasPngExtension(resolvedPath)) &&
          !resolvedPath.empty()) {
        const uint8_t pathIndex = registerBackgroundImagePath(resolvedPath);
        if (pathIndex != 0) {
          style.backgroundImagePath = pathIndex;
          style.defined.backgroundImage = 1;
        }
      }
    }
  } else if (!stylesheetDirectory.empty() && iequalsAscii(name, "background-repeat")) {
    const std::string_view repeatValue = stripTrailingImportant(value);
    if (iequalsAscii(repeatValue, "no-repeat")) {
      style.backgroundRepeat = CssBackgroundRepeat::NoRepeat;
      style.defined.backgroundRepeat = 1;
    } else if (iequalsAscii(repeatValue, "repeat")) {
      style.backgroundRepeat = CssBackgroundRepeat::Repeat;
      style.defined.backgroundRepeat = 1;
    }
  } else if (!stylesheetDirectory.empty() && iequalsAscii(name, "background-position")) {
    value = stripTrailingImportant(value);
    CssBackgroundPosition position;
    if (tryParseBackgroundPosition(value, position)) {
      style.backgroundPosition = position;
    } else {
      // A valid-but-unsupported placement must override an earlier supported
      // declaration conservatively so the renderer omits the decoration.
      style.backgroundPosition = CssBackgroundPosition::Unsupported;
    }
    style.defined.backgroundPosition = 1;
  } else if (!stylesheetDirectory.empty() && iequalsAscii(name, "background")) {
    // The shorthand can reset every background sub-property. We do not parse
    // it, but marking an unsupported reset prevents stale longhand values from
    // rendering after a later shorthand declaration.
    style.backgroundImagePath = 0;
    style.backgroundRepeat = CssBackgroundRepeat::Repeat;
    style.backgroundPosition = CssBackgroundPosition::Unsupported;
    style.defined.backgroundImage = 1;
    style.defined.backgroundRepeat = 1;
    style.defined.backgroundPosition = 1;
  }
}

CssStyle CssParser::parseDeclarations(const std::string_view declBlock, const std::string_view stylesheetDirectory) {
  CssStyle style;

  size_t start = 0;
  for (size_t i = 0; i <= declBlock.size(); ++i) {
    if (i == declBlock.size() || declBlock[i] == ';') {
      if (i > start) {
        parseDeclarationIntoStyle(declBlock.substr(start, i - start), style, stylesheetDirectory);
      }
      start = i + 1;
    }
  }

  return style;
}

bool CssParser::tryParseBackgroundImage(const std::string_view val, std::string& path) {
  const std::string_view trimmed = trimCssWhitespace(val);
  if (trimmed.size() < 6 || !hasAsciiPrefixInsensitive(trimmed, BACKGROUND_IMAGE_FUNCTION)) return false;
  size_t open = BACKGROUND_IMAGE_FUNCTION.size();
  while (open < trimmed.size() && isCssWhitespace(trimmed[open])) ++open;
  if (open >= trimmed.size() || trimmed[open] != '(' || trimmed.back() != ')') return false;
  std::string_view inside = trimCssWhitespace(trimmed.substr(open + 1, trimmed.size() - open - 2));
  if (inside.empty()) return false;
  if (inside.front() == '\'' || inside.front() == '"') {
    const char quote = inside.front();
    if (inside.size() < 2 || inside.back() != quote) return false;
    inside = trimCssWhitespace(inside.substr(1, inside.size() - 2));
  } else if (inside.find_first_of("'\"") != std::string_view::npos) {
    return false;
  }
  if (!isLocalBackgroundPath(inside)) return false;
  path.assign(inside.data(), inside.size());
  return true;
}

bool CssParser::tryParseBackgroundPosition(const std::string_view val, CssBackgroundPosition& position) {
  std::string_view tokens[2];
  size_t count = 0;
  forEachDelimitedToken(trimCssWhitespace(val), isCssWhitespace, [&](const std::string_view token) {
    if (count < 2) tokens[count++] = token;
  });
  if (count != 2) return false;

  const bool topFirst = iequalsAscii(tokens[0], "top");
  const bool topSecond = iequalsAscii(tokens[1], "top");
  const std::string_view horizontal = topFirst ? tokens[1] : (topSecond ? tokens[0] : std::string_view{});
  if (!topFirst && !topSecond) return false;
  if (iequalsAscii(horizontal, "left")) {
    position = CssBackgroundPosition::TopLeft;
  } else if (iequalsAscii(horizontal, "center")) {
    position = CssBackgroundPosition::TopCenter;
  } else if (iequalsAscii(horizontal, "right")) {
    position = CssBackgroundPosition::TopRight;
  } else {
    return false;
  }
  return true;
}

uint8_t CssParser::registerBackgroundImagePath(std::string path) {
  for (size_t i = 0; i < backgroundImagePaths_.size(); ++i) {
    if (backgroundImagePaths_[i] == path) return static_cast<uint8_t>(i + 1);
  }
  if (backgroundImagePaths_.size() >= MAX_BACKGROUND_IMAGE_PATHS || path.empty() ||
      path.size() > MAX_BACKGROUND_IMAGE_PATH_LENGTH) {
    LOG_DBG("CSS", "Background path table full or path too long");
    return 0;
  }
  backgroundImagePaths_.push_back(std::move(path));
  return static_cast<uint8_t>(backgroundImagePaths_.size());
}

// Rule processing

void CssParser::processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style) {
  // Skip rules that don't define any supported properties to save RAM.
  if (!style.defined.anySet()) {
    return;
  }

  // Check if we've reached the rule limit before processing
  if (rulesBySelector_.size() >= MAX_RULES) {
    LOG_DBG("CSS", "Reached max rules limit (%zu), stopping CSS parsing", MAX_RULES);
    return;
  }

  // Walk comma-separated selectors in place — no vector allocation. Selectors
  // with unsupported syntax (combinators, attributes, pseudo, etc.) are skipped
  // silently; the only heap allocation per kept selector is the std::string
  // map key, which is unavoidable since the map owns its keys.
  bool limitReached = false;
  forEachDelimitedToken(
      selectorGroup, [](char c) { return c == ','; },
      [&](std::string_view sel) {
        if (limitReached) return;

        if (sel.size() > MAX_SELECTOR_LENGTH) {
          LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", sel.size(), MAX_SELECTOR_LENGTH);
          return;
        }

        // TODO: Support richer CSS selector syntax in the future. For now we only
        // handle `tag`, `.class`, or `tag.class`. Reject anything containing a
        // character that introduces unsupported syntax:
        //   '+'  adjacent sibling combinator
        //   '>'  child combinator
        //   '['  attribute selector
        //   ':'  pseudo class/element
        //   '#'  ID selector
        //   '~'  general sibling combinator
        //   '*'  wildcard
        //   ' '  descendant combinator
        // Single-pass scan via find_first_of instead of eight sequential find() calls.
        constexpr std::string_view kUnsupportedSelectorChars = "+>[:#~* ";
        if (sel.find_first_of(kUnsupportedSelectorChars) != std::string_view::npos) return;

        // Skip if this would exceed the rule limit
        if (rulesBySelector_.size() >= MAX_RULES) {
          LOG_DBG("CSS", "Reached max rules limit, stopping selector processing");
          limitReached = true;
          return;
        }

        // Store or merge with existing. Hash/equal are case-insensitive, so two
        // selectors that differ only in ASCII case collide on insert and merge.
        auto it = rulesBySelector_.find(sel);
        if (it != rulesBySelector_.end()) {
          it->second.applyOver(style);
        } else {
          rulesBySelector_.emplace(std::string(sel), style);
        }
      });
}

// Main parsing entry point

bool CssParser::loadFromStream(HalFile& source, const std::string_view stylesheetPath) {
  if (!source) {
    LOG_ERR("CSS", "Cannot read from invalid file");
    return false;
  }

  size_t totalRead = 0;

  // Use stack-allocated buffers for parsing to avoid heap reallocations
  StackBuffer selector;
  StackBuffer declBuffer;

  bool inComment = false;
  bool maybeSlash = false;
  bool prevStar = false;

  bool inAtRule = false;
  int atDepth = 0;

  int bodyDepth = 0;
  bool skippingRule = false;
  CssStyle currentStyle;
  backgroundImagePaths_.reserve(BACKGROUND_IMAGE_PATH_RESERVE);
  std::string stylesheetDirectory;
  if (!stylesheetPath.empty()) {
    const size_t slash = stylesheetPath.find_last_of('/');
    if (slash != std::string_view::npos) stylesheetDirectory.assign(stylesheetPath.substr(0, slash + 1));
  }

  auto handleChar = [&](const char c) {
    if (inAtRule) {
      if (c == '{') {
        ++atDepth;
      } else if (c == '}') {
        if (atDepth > 0) --atDepth;
        if (atDepth == 0) inAtRule = false;
      } else if (c == ';' && atDepth == 0) {
        inAtRule = false;
      }
      return;
    }

    if (bodyDepth == 0) {
      if (selector.empty() && isCssWhitespace(c)) {
        return;
      }
      if (c == '@' && selector.empty()) {
        inAtRule = true;
        atDepth = 0;
        return;
      }
      if (c == '{') {
        bodyDepth = 1;
        currentStyle = CssStyle{};
        declBuffer.clear();
        if (selector.size() > MAX_SELECTOR_LENGTH * 4) {
          skippingRule = true;
        }
        return;
      }
      selector.push_back(c);
      return;
    }

    // bodyDepth > 0
    if (c == '{') {
      ++bodyDepth;
      return;
    }
    if (c == '}') {
      --bodyDepth;
      if (bodyDepth == 0) {
        if (!skippingRule && !declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer, currentStyle, stylesheetDirectory);
        }
        if (!skippingRule) {
          processRuleBlockWithStyle(selector, currentStyle);
        }
        selector.clear();
        declBuffer.clear();
        skippingRule = false;
        return;
      }
      return;
    }
    if (bodyDepth > 1) {
      return;
    }
    if (!skippingRule) {
      if (c == ';') {
        if (!declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer, currentStyle, stylesheetDirectory);
          declBuffer.clear();
        }
      } else {
        declBuffer.push_back(c);
      }
    }
  };

  char buffer[READ_BUFFER_SIZE];
  while (source.available()) {
    int bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;

    totalRead += static_cast<size_t>(bytesRead);

    for (int i = 0; i < bytesRead; ++i) {
      const char c = buffer[i];

      if (inComment) {
        if (prevStar && c == '/') {
          inComment = false;
          prevStar = false;
          continue;
        }
        prevStar = c == '*';
        continue;
      }

      if (maybeSlash) {
        if (c == '*') {
          inComment = true;
          maybeSlash = false;
          prevStar = false;
          continue;
        }
        handleChar('/');
        maybeSlash = false;
        // fall through to process current char
      }

      if (c == '/') {
        maybeSlash = true;
        continue;
      }

      handleChar(c);
    }
  }

  if (maybeSlash) {
    handleChar('/');
  }

  LOG_DBG("CSS", "Parsed %zu rules from %zu bytes", rulesBySelector_.size(), totalRead);
  return true;
}

// Style resolution

CssStyle CssParser::resolveStyle(std::string_view tagName, std::string_view classAttr) const {
  static bool lowHeapWarningLogged = false;
  if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_CSS) {
    if (!lowHeapWarningLogged) {
      lowHeapWarningLogged = true;
      LOG_DBG("CSS", "Warning: low heap (%u bytes) below MIN_FREE_HEAP_FOR_CSS (%u), returning empty style",
              ESP.getFreeHeap(), static_cast<unsigned>(MIN_FREE_HEAP_FOR_CSS));
    }
    return CssStyle{};
  }

  CssStyle result;

  // 1. Apply element-level style (lowest priority). The map's hash/equal are
  // case-insensitive, so the raw tagName view can be used as the lookup key.
  if (auto it = rulesBySelector_.find(tagName); it != rulesBySelector_.end()) {
    result.applyOver(it->second);
  }

  if (classAttr.empty()) return result;

  // TODO: Support combinations of classes (e.g. style on .class1.class2)
  // 2. Apply class styles (medium priority). The transparent hash/equal accept
  // a CompositeKey, so we never materialize the concatenation.
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) {
    if (auto it = rulesBySelector_.find(CompositeKey{".", cls}); it != rulesBySelector_.end()) {
      result.applyOver(it->second);
    }
  });

  // TODO: Support combinations of classes (e.g. style on p.class1.class2)
  // 3. Apply element.class styles (higher priority).
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) {
    if (auto it = rulesBySelector_.find(CompositeKey{tagName, ".", cls}); it != rulesBySelector_.end()) {
      result.applyOver(it->second);
    }
  });

  return result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(const std::string_view styleValue) {
  CssParser parser("");
  return parser.parseDeclarations(styleValue);
}

std::string_view CssParser::backgroundImagePath(const CssStyle& style) const {
  if (!style.hasBackgroundImage() || style.backgroundImagePath > backgroundImagePaths_.size()) return {};
  return backgroundImagePaths_[style.backgroundImagePath - 1];
}

// Cache serialization

// Cache file name (version is CssParser::CSS_CACHE_VERSION)
constexpr char rulesCache[] = "/css_rules.cache";

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
}

bool CssParser::saveToCache() const {
  if (cachePath.empty()) {
    return false;
  }

  HalFile file;
  if (!Storage.openFileForWrite("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  // Write version
  file.write(CssParser::CSS_CACHE_VERSION);

  const auto pathCount = static_cast<uint16_t>(backgroundImagePaths_.size());
  file.write(reinterpret_cast<const uint8_t*>(&pathCount), sizeof(pathCount));
  for (const auto& path : backgroundImagePaths_) {
    const auto pathLen = static_cast<uint16_t>(path.size());
    file.write(reinterpret_cast<const uint8_t*>(&pathLen), sizeof(pathLen));
    file.write(reinterpret_cast<const uint8_t*>(path.data()), pathLen);
  }

  // Write rule count
  const auto ruleCount = static_cast<uint16_t>(rulesBySelector_.size());
  file.write(reinterpret_cast<const uint8_t*>(&ruleCount), sizeof(ruleCount));

  // Write each rule: selector string + CssStyle fields
  for (const auto& pair : rulesBySelector_) {
    // Write selector string (length-prefixed)
    const auto selectorLen = static_cast<uint16_t>(pair.first.size());
    file.write(reinterpret_cast<const uint8_t*>(&selectorLen), sizeof(selectorLen));
    file.write(reinterpret_cast<const uint8_t*>(pair.first.data()), selectorLen);

    // Write CssStyle fields (all are POD types)
    const CssStyle& style = pair.second;
    file.write(static_cast<uint8_t>(style.textAlign));
    file.write(static_cast<uint8_t>(style.fontStyle));
    file.write(static_cast<uint8_t>(style.fontWeight));
    file.write(static_cast<uint8_t>(style.textDecoration));
    file.write(static_cast<uint8_t>(style.direction));

    // Write CssLength fields (value + unit)
    auto writeLength = [&file](const CssLength& len) {
      file.write(reinterpret_cast<const uint8_t*>(&len.value), sizeof(len.value));
      file.write(static_cast<uint8_t>(len.unit));
    };

    writeLength(style.textIndent);
    writeLength(style.marginTop);
    writeLength(style.marginBottom);
    writeLength(style.marginLeft);
    writeLength(style.marginRight);
    writeLength(style.paddingTop);
    writeLength(style.paddingBottom);
    writeLength(style.paddingLeft);
    writeLength(style.paddingRight);
    writeLength(style.imageHeight);
    writeLength(style.imageWidth);
    file.write(static_cast<uint8_t>(style.display));
    file.write(static_cast<uint8_t>(style.verticalAlign));
    file.write(style.backgroundImagePath);
    file.write(static_cast<uint8_t>(style.backgroundRepeat));
    file.write(static_cast<uint8_t>(style.backgroundPosition));

    // Write defined flags as uint32_t
    uint32_t definedBits = 0;
    if (style.defined.textAlign) definedBits |= 1 << 0;
    if (style.defined.fontStyle) definedBits |= 1 << 1;
    if (style.defined.fontWeight) definedBits |= 1 << 2;
    if (style.defined.textDecoration) definedBits |= 1 << 3;
    if (style.defined.textIndent) definedBits |= 1 << 4;
    if (style.defined.marginTop) definedBits |= 1 << 5;
    if (style.defined.marginBottom) definedBits |= 1 << 6;
    if (style.defined.marginLeft) definedBits |= 1 << 7;
    if (style.defined.marginRight) definedBits |= 1 << 8;
    if (style.defined.paddingTop) definedBits |= 1 << 9;
    if (style.defined.paddingBottom) definedBits |= 1 << 10;
    if (style.defined.paddingLeft) definedBits |= 1 << 11;
    if (style.defined.paddingRight) definedBits |= 1 << 12;
    if (style.defined.imageHeight) definedBits |= 1 << 13;
    if (style.defined.imageWidth) definedBits |= 1 << 14;
    if (style.defined.display) definedBits |= 1 << 15;
    if (style.defined.direction) definedBits |= 1 << 16;
    if (style.defined.verticalAlign) definedBits |= 1 << 17;
    if (style.defined.backgroundImage) definedBits |= 1u << 18;
    if (style.defined.backgroundRepeat) definedBits |= 1u << 19;
    if (style.defined.backgroundPosition) definedBits |= 1u << 20;
    file.write(reinterpret_cast<const uint8_t*>(&definedBits), sizeof(definedBits));
  }

  LOG_DBG("CSS", "Saved %u rules to cache", ruleCount);
  return true;
}

bool CssParser::loadFromCache() {
  if (cachePath.empty()) {
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  // Clear existing rules
  clear();

  // Read and verify version
  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != CssParser::CSS_CACHE_VERSION) {
    LOG_DBG("CSS", "Cache version mismatch (got %u, expected %u), removing stale cache for rebuild", version,
            CssParser::CSS_CACHE_VERSION);
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }

  auto hasRemainingBytes = [&file](const size_t neededBytes) {
    return static_cast<size_t>(file.available()) >= neededBytes;
  };

  uint16_t pathCount = 0;
  if (!hasRemainingBytes(sizeof(pathCount)) || file.read(&pathCount, sizeof(pathCount)) != sizeof(pathCount) ||
      pathCount > MAX_BACKGROUND_IMAGE_PATHS) {
    LOG_DBG("CSS", "Invalid background path count in cache");
    clear();
    return false;
  }
  backgroundImagePaths_.reserve(pathCount);
  for (uint16_t i = 0; i < pathCount; ++i) {
    uint16_t pathLen = 0;
    if (!hasRemainingBytes(sizeof(pathLen)) || file.read(&pathLen, sizeof(pathLen)) != sizeof(pathLen) ||
        pathLen == 0 || pathLen > MAX_BACKGROUND_IMAGE_PATH_LENGTH || !hasRemainingBytes(pathLen)) {
      LOG_DBG("CSS", "Invalid background path length in cache");
      clear();
      return false;
    }
    std::string path(pathLen, '\0');
    if (file.read(path.data(), pathLen) != pathLen || !isLocalBackgroundPath(path) ||
        (!FsHelpers::hasJpgExtension(path) && !FsHelpers::hasPngExtension(path)) ||
        FsHelpers::normalisePath(path) != path ||
        std::find(backgroundImagePaths_.begin(), backgroundImagePaths_.end(), path) != backgroundImagePaths_.end()) {
      LOG_DBG("CSS", "Invalid or duplicate background path in cache");
      clear();
      return false;
    }
    backgroundImagePaths_.push_back(std::move(path));
  }

  // Read rule count
  uint16_t ruleCount = 0;
  if (file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount)) {
    return false;
  }

  if (ruleCount > MAX_RULES) {
    LOG_DBG("CSS", "Invalid cache rule count (%u > %zu)", ruleCount, MAX_RULES);
    rulesBySelector_.clear();
    return false;
  }

  // Size the bucket array up front to avoid incremental rehashes while loading rules.
  rulesBySelector_.reserve(ruleCount);

  constexpr size_t CSS_LENGTH_FIELD_COUNT = 11;
  constexpr size_t CSS_LENGTH_BYTES = sizeof(float) + sizeof(uint8_t);
  constexpr size_t CSS_FIXED_STYLE_BYTES =
      5 * sizeof(uint8_t) + (CSS_LENGTH_FIELD_COUNT * CSS_LENGTH_BYTES) + 5 * sizeof(uint8_t) + sizeof(uint32_t);

  // Read each rule
  for (uint16_t i = 0; i < ruleCount; ++i) {
    // Read selector string
    uint16_t selectorLen = 0;
    if (!hasRemainingBytes(sizeof(selectorLen))) {
      rulesBySelector_.clear();
      return false;
    }
    if (file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen)) {
      rulesBySelector_.clear();
      return false;
    }

    if (selectorLen == 0 || selectorLen > MAX_SELECTOR_LENGTH || !hasRemainingBytes(selectorLen)) {
      LOG_DBG("CSS", "Invalid selector length in cache: %u", selectorLen);
      rulesBySelector_.clear();
      return false;
    }

    std::string selector;
    selector.resize(selectorLen);
    if (file.read(&selector[0], selectorLen) != selectorLen) {
      rulesBySelector_.clear();
      return false;
    }

    if (!hasRemainingBytes(CSS_FIXED_STYLE_BYTES)) {
      LOG_DBG("CSS", "Truncated CSS cache while reading style payload");
      rulesBySelector_.clear();
      return false;
    }

    // Read CssStyle fields
    CssStyle style;
    uint8_t enumVal;

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.textAlign = static_cast<CssTextAlign>(enumVal);
    if (enumVal > static_cast<uint8_t>(CssTextAlign::None)) {
      rulesBySelector_.clear();
      return false;
    }

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.fontStyle = static_cast<CssFontStyle>(enumVal);
    if (enumVal > static_cast<uint8_t>(CssFontStyle::Italic)) {
      rulesBySelector_.clear();
      return false;
    }

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.fontWeight = static_cast<CssFontWeight>(enumVal);
    if (enumVal > static_cast<uint8_t>(CssFontWeight::Bold)) {
      rulesBySelector_.clear();
      return false;
    }

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    if ((enumVal & ~CSS_TEXT_DECORATION_MASK) != 0) {
      rulesBySelector_.clear();
      return false;
    }
    style.textDecoration = static_cast<CssTextDecoration>(enumVal);

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.direction = static_cast<CssTextDirection>(enumVal);
    if (enumVal > static_cast<uint8_t>(CssTextDirection::Rtl)) {
      rulesBySelector_.clear();
      return false;
    }

    // Read CssLength fields
    auto readLength = [&file](CssLength& len) -> bool {
      if (file.read(&len.value, sizeof(len.value)) != sizeof(len.value)) {
        return false;
      }
      uint8_t unitVal;
      if (file.read(&unitVal, 1) != 1) {
        return false;
      }
      if (unitVal > static_cast<uint8_t>(CssUnit::Percent) || !std::isfinite(len.value)) {
        return false;
      }
      len.unit = static_cast<CssUnit>(unitVal);
      return true;
    };

    if (!readLength(style.textIndent) || !readLength(style.marginTop) || !readLength(style.marginBottom) ||
        !readLength(style.marginLeft) || !readLength(style.marginRight) || !readLength(style.paddingTop) ||
        !readLength(style.paddingBottom) || !readLength(style.paddingLeft) || !readLength(style.paddingRight) ||
        !readLength(style.imageHeight) || !readLength(style.imageWidth)) {
      rulesBySelector_.clear();
      return false;
    }

    // Read display value
    uint8_t displayVal;
    if (file.read(&displayVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.display = static_cast<CssDisplay>(displayVal);
    if (displayVal > static_cast<uint8_t>(CssDisplay::None)) {
      rulesBySelector_.clear();
      return false;
    }

    // Read verticalAlign value
    uint8_t verticalAlignVal;
    if (file.read(&verticalAlignVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.verticalAlign = static_cast<CssVerticalAlign>(verticalAlignVal);
    if (verticalAlignVal > static_cast<uint8_t>(CssVerticalAlign::Sub)) {
      rulesBySelector_.clear();
      return false;
    }

    if (file.read(&style.backgroundImagePath, sizeof(style.backgroundImagePath)) != sizeof(style.backgroundImagePath) ||
        file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.backgroundRepeat = static_cast<CssBackgroundRepeat>(enumVal);
    if (enumVal > static_cast<uint8_t>(CssBackgroundRepeat::NoRepeat) || file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.backgroundPosition = static_cast<CssBackgroundPosition>(enumVal);
    if (enumVal > static_cast<uint8_t>(CssBackgroundPosition::Unsupported)) {
      rulesBySelector_.clear();
      return false;
    }

    // Read defined flags
    uint32_t definedBits = 0;
    if (file.read(&definedBits, sizeof(definedBits)) != sizeof(definedBits)) {
      rulesBySelector_.clear();
      return false;
    }
    style.defined.textAlign = (definedBits & 1 << 0) != 0;
    style.defined.fontStyle = (definedBits & 1 << 1) != 0;
    style.defined.fontWeight = (definedBits & 1 << 2) != 0;
    style.defined.textDecoration = (definedBits & 1 << 3) != 0;
    style.defined.textIndent = (definedBits & 1 << 4) != 0;
    style.defined.marginTop = (definedBits & 1 << 5) != 0;
    style.defined.marginBottom = (definedBits & 1 << 6) != 0;
    style.defined.marginLeft = (definedBits & 1 << 7) != 0;
    style.defined.marginRight = (definedBits & 1 << 8) != 0;
    style.defined.paddingTop = (definedBits & 1 << 9) != 0;
    style.defined.paddingBottom = (definedBits & 1 << 10) != 0;
    style.defined.paddingLeft = (definedBits & 1 << 11) != 0;
    style.defined.paddingRight = (definedBits & 1 << 12) != 0;
    style.defined.imageHeight = (definedBits & 1 << 13) != 0;
    style.defined.imageWidth = (definedBits & 1 << 14) != 0;
    style.defined.display = (definedBits & 1 << 15) != 0;
    style.defined.direction = (definedBits & 1 << 16) != 0;
    style.defined.verticalAlign = (definedBits & 1 << 17) != 0;
    style.defined.backgroundImage = (definedBits & 1u << 18) != 0;
    style.defined.backgroundRepeat = (definedBits & 1u << 19) != 0;
    style.defined.backgroundPosition = (definedBits & 1u << 20) != 0;

    constexpr uint32_t KNOWN_DEFINED_BITS = (1u << 21) - 1;
    if ((definedBits & ~KNOWN_DEFINED_BITS) != 0 || (style.backgroundImagePath > pathCount) ||
        (!style.defined.backgroundImage && style.backgroundImagePath != 0) ||
        (!style.defined.backgroundRepeat && style.backgroundRepeat != CssBackgroundRepeat::Repeat) ||
        (!style.defined.backgroundPosition && style.backgroundPosition != CssBackgroundPosition::TopLeft)) {
      LOG_DBG("CSS", "Invalid background metadata in cache");
      rulesBySelector_.clear();
      return false;
    }

    if (!rulesBySelector_.emplace(std::move(selector), style).second) {
      LOG_DBG("CSS", "Duplicate selector in cache");
      rulesBySelector_.clear();
      return false;
    }
  }

  if (file.available() != 0) {
    LOG_DBG("CSS", "Trailing bytes in cache");
    clear();
    return false;
  }

  LOG_DBG("CSS", "Loaded %u rules from cache", ruleCount);
  return true;
}
