#!/usr/bin/env python3
"""Generate a deterministic EPUB for bounded chapter-heading CSS backgrounds."""

import struct
import zipfile
import zlib
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
OUTPUT = REPO_ROOT / "test" / "epubs" / "test_css_background_heading.epub"
JPEG_SOURCE_EPUB = REPO_ROOT / "test" / "epubs" / "test_jpeg_images.epub"
FIXED_TIME = (2020, 1, 1, 0, 0, 0)


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))


def make_icon_png(width: int = 64, height: int = 64) -> bytes:
    """Create a simple monochrome diamond icon using only the standard library."""
    rows = bytearray()
    center_x = width // 2
    center_y = height // 2
    for y in range(height):
        rows.append(0)  # PNG filter: none
        for x in range(width):
            distance = abs(x - center_x) + abs(y - center_y)
            border = 27 <= distance <= 30
            inner = distance <= 9
            rows.append(0 if border or inner else 255)
    signature = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    return signature + png_chunk(b"IHDR", ihdr) + png_chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + png_chunk(b"IEND", b"")


def chapter(title: str, body: str) -> str:
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
<head>
  <title>{title}</title>
  <link rel="stylesheet" type="text/css" href="Styles/chapter.css"/>
</head>
<body>
{body}
</body>
</html>
"""


CSS = """body { margin: 0; padding: 0; }
p { margin: 0 0 0.45em; }
.jpeg-heading {
  background-image: url(../Images/heading.jpg);
  background-position: center top;
  background-repeat: no-repeat;
  padding: 280px 0 0;
}
.png-heading {
  background-image: url('../Images/icon.png');
  background-position: top center;
  background-repeat: no-repeat;
  padding: 80px 0 0;
}
.unsupported-repeat {
  background-image: url(../Images/icon.png);
  background-position: center top;
  background-repeat: repeat;
  padding: 80px 0 0;
}
.unsupported-position {
  background-image: url(../Images/icon.png);
  background-position: center bottom;
  background-repeat: no-repeat;
  padding: 80px 0 0;
}
"""


FILLER = (
    "This paragraph intentionally fills the page so the following decorated heading "
    "can exercise the keep-together page-break behavior on the device."
)


CHAPTERS = [
    chapter(
        "JPEG centered heading",
        """<h1 class="jpeg-heading">JPEG background</h1>
<p>PASS: one generated test image appears centered above this heading and the text begins below it.</p>
<p>FAIL: the image is absent, repeated, overlaps the title, or appears as a placeholder.</p>""",
    ),
    chapter(
        "PNG centered heading",
        """<h2 class="png-heading">PNG background</h2>
<p>PASS: a small diamond icon appears once above this heading.</p>
<p>The inline copy below must also render, proving ordinary image handling is unchanged.</p>
<img src="Images/icon.png" alt="inline diamond icon"/>""",
    ),
    chapter(
        "Heading near page end",
        "\n".join(f"<p>{FILLER} Line {index + 1}.</p>" for index in range(12))
        + """
<h2 class="png-heading">Keep icon and heading together</h2>
<p>PASS: the icon and heading start together on one page; the icon must not remain alone on the preceding page.</p>""",
    ),
    chapter(
        "Unsupported backgrounds",
        """<h2 class="unsupported-repeat">No repeated icon</h2>
<p>PASS: no decorative icon or placeholder appears above the repeat-background heading.</p>
<h2 class="unsupported-position">No bottom-position icon</h2>
<p>PASS: no decorative icon or placeholder appears above the bottom-position heading.</p>""",
    ),
]


CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
"""


def content_opf() -> str:
    chapter_items = "\n".join(
        f'    <item id="ch{index}" href="chapter{index}.xhtml" media-type="application/xhtml+xml"/>'
        for index in range(1, len(CHAPTERS) + 1)
    )
    spine_items = "\n".join(f'    <itemref idref="ch{index}"/>' for index in range(1, len(CHAPTERS) + 1))
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="book-id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="book-id">crosspoint-css-background-heading-test</dc:identifier>
    <dc:title>CrossPoint CSS Background Heading Test</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
{chapter_items}
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="css" href="Styles/chapter.css" media-type="text/css"/>
    <item id="jpeg" href="Images/heading.jpg" media-type="image/jpeg"/>
    <item id="png" href="Images/icon.png" media-type="image/png"/>
  </manifest>
  <spine>
{spine_items}
  </spine>
</package>
"""


def nav_xhtml() -> str:
    links = "\n".join(
        f'      <li><a href="chapter{index}.xhtml">Test {index}</a></li>'
        for index in range(1, len(CHAPTERS) + 1)
    )
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Contents</title></head>
<body><nav epub:type="toc"><ol>
{links}
</ol></nav></body>
</html>
"""


def write_entry(archive: zipfile.ZipFile, name: str, data: bytes | str, *, stored: bool = False) -> None:
    info = zipfile.ZipInfo(name, FIXED_TIME)
    info.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    archive.writestr(info, data.encode("utf-8") if isinstance(data, str) else data)


def main() -> None:
    with zipfile.ZipFile(JPEG_SOURCE_EPUB) as source:
        heading_jpeg = source.read("OEBPS/images/jpeg_format.jpg")

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(OUTPUT, "w") as archive:
        write_entry(archive, "mimetype", "application/epub+zip", stored=True)
        write_entry(archive, "META-INF/container.xml", CONTAINER)
        write_entry(archive, "OEBPS/content.opf", content_opf())
        write_entry(archive, "OEBPS/nav.xhtml", nav_xhtml())
        write_entry(archive, "OEBPS/Styles/chapter.css", CSS)
        write_entry(archive, "OEBPS/Images/heading.jpg", heading_jpeg)
        write_entry(archive, "OEBPS/Images/icon.png", make_icon_png())
        for index, contents in enumerate(CHAPTERS, start=1):
            write_entry(archive, f"OEBPS/chapter{index}.xhtml", contents)

    print(f"Wrote {OUTPUT}")


if __name__ == "__main__":
    main()
