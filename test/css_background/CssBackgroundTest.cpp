#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "Epub/css/CssParser.h"

namespace {

std::string readFixture() {
  std::ifstream input(FIXTURE_PATH);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

TEST(CssBackground, ParsesBoundedChapterHeadingSubset) {
  CssParser parser("");
  HalFile source(readFixture());
  ASSERT_TRUE(parser.loadFromStream(source, "OEBPS/Styles/chapter.css"));

  const CssStyle heading = parser.resolveStyle("h3", "aud_h3");
  ASSERT_TRUE(heading.hasBackgroundImage());
  EXPECT_EQ(parser.backgroundImagePath(heading), "OEBPS/Images/heading.jpg");
  EXPECT_TRUE(heading.hasBackgroundRepeat());
  EXPECT_EQ(heading.backgroundRepeat, CssBackgroundRepeat::NoRepeat);
  EXPECT_TRUE(heading.hasBackgroundPosition());
  EXPECT_EQ(heading.backgroundPosition, CssBackgroundPosition::TopCenter);
  EXPECT_EQ(heading.paddingTop.value, 120.0f);

  const CssStyle samePath = parser.resolveStyle("div", "same-path");
  EXPECT_EQ(parser.backgroundImagePath(samePath), "OEBPS/Images/heading.jpg");
  EXPECT_EQ(samePath.backgroundImagePath, heading.backgroundImagePath);

  const CssStyle clearsImage = parser.resolveStyle("div", "clears-image");
  EXPECT_TRUE(clearsImage.defined.backgroundImage);
  EXPECT_FALSE(clearsImage.hasBackgroundImage());
  EXPECT_EQ(clearsImage.backgroundImagePath, 0);
}

TEST(CssBackground, RejectsRemoteShorthandAndUnsafeValues) {
  CssParser parser("");
  HalFile source(readFixture());
  ASSERT_TRUE(parser.loadFromStream(source, "OEBPS/Styles/chapter.css"));

  const CssStyle unsupported = parser.resolveStyle("div", "unsupported");
  EXPECT_FALSE(unsupported.hasBackgroundImage());
  EXPECT_TRUE(unsupported.hasBackgroundRepeat());
  EXPECT_EQ(unsupported.backgroundRepeat, CssBackgroundRepeat::Repeat);
  EXPECT_TRUE(unsupported.hasBackgroundPosition());
  EXPECT_EQ(unsupported.backgroundPosition, CssBackgroundPosition::Unsupported);

  EXPECT_FALSE(parser.resolveStyle("div", "encoded-remote").hasBackgroundImage());
  EXPECT_FALSE(parser.resolveStyle("div", "encoded-absolute").hasBackgroundImage());
  EXPECT_FALSE(parser.resolveStyle("div", "excess-parent").hasBackgroundImage());

  const CssStyle inlineStyle = CssParser::parseInlineStyle(
      "background-image:url(../Images/inline.jpg); background-repeat:no-repeat; background-position:top left");
  EXPECT_FALSE(inlineStyle.hasBackgroundImage());
  EXPECT_FALSE(inlineStyle.hasBackgroundRepeat());
  EXPECT_FALSE(inlineStyle.hasBackgroundPosition());
}

TEST(CssBackground, AcceptsBothTopKeywordOrdersAndHorizontalPositions) {
  CssParser parser("");
  HalFile source(
      ".left{background-image:url(../Images/l.png);background-repeat:no-repeat;background-position:left top;}"
      ".center{background-image:url('../Images/c.jpg');background-repeat:no-repeat;background-position:top center;}"
      ".right{background-image:url(../Images/r.png);background-repeat:no-repeat;background-position:right top;}");
  ASSERT_TRUE(parser.loadFromStream(source, "OPS/Css/book.css"));

  EXPECT_EQ(parser.resolveStyle("h1", "left").backgroundPosition, CssBackgroundPosition::TopLeft);
  EXPECT_EQ(parser.resolveStyle("h2", "center").backgroundPosition, CssBackgroundPosition::TopCenter);
  EXPECT_EQ(parser.resolveStyle("h3", "right").backgroundPosition, CssBackgroundPosition::TopRight);
  EXPECT_EQ(parser.backgroundImagePath(parser.resolveStyle("h1", "left")), "OPS/Images/l.png");
}

TEST(CssBackground, LaterValidUnsupportedDeclarationsDisableRendering) {
  CssParser parser("");
  HalFile source(
      ".repeat{background-image:url(../Images/x.png);background-repeat:no-repeat;background-repeat:repeat;"
      "background-position:left top;}"
      ".bottom{background-image:url(../Images/x.png);background-repeat:no-repeat;background-position:left top;"
      "background-position:center bottom;}"
      ".shorthand{background-image:url(../Images/x.png);background-repeat:no-repeat;background-position:left top;"
      "background:url(../Images/y.png) no-repeat center top;}");
  ASSERT_TRUE(parser.loadFromStream(source, "OPS/Css/book.css"));

  EXPECT_EQ(parser.resolveStyle("h1", "repeat").backgroundRepeat, CssBackgroundRepeat::Repeat);
  EXPECT_EQ(parser.resolveStyle("h1", "bottom").backgroundPosition, CssBackgroundPosition::Unsupported);
  EXPECT_FALSE(parser.resolveStyle("h1", "shorthand").hasBackgroundImage());
}

TEST(CssBackground, ImportantSuffixIsAcceptedForSupportedLonghands) {
  CssParser parser("");
  HalFile source(".x{background-image:url(../Images/x.png)!important;"
                 "background-repeat:no-repeat !important;background-position:center top!important;}");
  ASSERT_TRUE(parser.loadFromStream(source, "OPS/Css/book.css"));
  const CssStyle style = parser.resolveStyle("h1", "x");
  EXPECT_EQ(parser.backgroundImagePath(style), "OPS/Images/x.png");
  EXPECT_EQ(style.backgroundRepeat, CssBackgroundRepeat::NoRepeat);
  EXPECT_EQ(style.backgroundPosition, CssBackgroundPosition::TopCenter);
}

TEST(CssBackground, EnforcesPathLengthAndResourceCountBounds) {
  CssParser parser("");
  std::string css = ".long{background-image:url(../Images/" + std::string(500, 'a') + ".png);}";
  for (int index = 0; index < 256; ++index) {
    css += ".p" + std::to_string(index) + "{background-image:url(../Images/p" + std::to_string(index) +
           ".png);background-repeat:no-repeat;background-position:left top;}";
  }
  HalFile source(std::move(css));
  ASSERT_TRUE(parser.loadFromStream(source, "OPS/Css/book.css"));

  EXPECT_FALSE(parser.resolveStyle("h1", "long").hasBackgroundImage());
  EXPECT_TRUE(parser.resolveStyle("h1", "p254").hasBackgroundImage());
  EXPECT_FALSE(parser.resolveStyle("h1", "p255").hasBackgroundImage());
}

TEST(CssBackground, CacheRoundTripAndCorruptionRejection) {
  Storage.clear();
  CssParser writer("/book");
  HalFile source(readFixture());
  ASSERT_TRUE(writer.loadFromStream(source, "OEBPS/Styles/chapter.css"));
  ASSERT_TRUE(writer.saveToCache());

  CssParser reader("/book");
  ASSERT_TRUE(reader.loadFromCache());
  const CssStyle roundTrip = reader.resolveStyle("h3", "aud_h3");
  EXPECT_EQ(reader.backgroundImagePath(roundTrip), "OEBPS/Images/heading.jpg");
  EXPECT_EQ(roundTrip.backgroundPosition, CssBackgroundPosition::TopCenter);

  auto& cache = Storage.files.at("/book/css_rules.cache");
  cache.resize(cache.size() - 1);
  CssParser truncated("/book");
  EXPECT_FALSE(truncated.loadFromCache());

  Storage.clear();
  CssParser invalidWriter("/book");
  HalFile invalidSource(".x{background-image:url(../Images/x.png);background-repeat:no-repeat;"
                        "background-position:left top;}");
  ASSERT_TRUE(invalidWriter.loadFromStream(invalidSource, "OPS/Css/book.css"));
  ASSERT_TRUE(invalidWriter.saveToCache());
  auto& invalidCache = Storage.files.at("/book/css_rules.cache");

  size_t cursor = 1;
  uint16_t pathCount = 0;
  std::memcpy(&pathCount, invalidCache.data() + cursor, sizeof(pathCount));
  cursor += sizeof(pathCount);
  ASSERT_EQ(pathCount, 1);
  uint16_t pathLength = 0;
  std::memcpy(&pathLength, invalidCache.data() + cursor, sizeof(pathLength));
  cursor += sizeof(pathLength) + pathLength;
  cursor += sizeof(uint16_t);  // rule count
  uint16_t selectorLength = 0;
  std::memcpy(&selectorLength, invalidCache.data() + cursor, sizeof(selectorLength));
  cursor += sizeof(selectorLength) + selectorLength;
  constexpr size_t BYTES_BEFORE_BACKGROUND_ID = 5 + 11 * (sizeof(float) + sizeof(uint8_t)) + 2;
  invalidCache[cursor + BYTES_BEFORE_BACKGROUND_ID] = static_cast<char>(pathCount + 1);

  CssParser invalidId("/book");
  EXPECT_FALSE(invalidId.loadFromCache());
}

}  // namespace
