# Builds the PDF versions of the manuals: docs/manual.md -> docs/DJAP-Manual.pdf and docs/technical.md ->
# docs/DJAP-Technical-Manual.pdf. Markdown -> HTML (markdown-it-py, with GitHub-style heading anchors so the contents
# links work) -> PDF with headless Chrome or Edge (a throwaway profile; the user's own browser profile is not used).
# Each PDF starts with a cover page: the logo (assets/logo.png), the title and what the manual covers.
# Usage: python -I tools\make_manuals.py [version]   (the version, if given, is printed on the cover)
import html
import os
import re
import shutil
import subprocess
import sys
import tempfile

from markdown_it import MarkdownIt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "docs")
LOGO = "../assets/logo.png"  # relative to docs/, where the page is printed
MANUALS = [("manual.md", "DJAP-Manual.pdf", "DJAP user manual",
            "Installing, connecting and using DJAP: every window, control and setting"),
           ("technical.md", "DJAP-Technical-Manual.pdf", "DJAP technical manual",
            "How the audio processing works: every stage, its formulas, measurements and sources")]

CSS = """
@page { size: A4; margin: 16mm 15mm 16mm 15mm; }
body { font-family: "Segoe UI", Arial, sans-serif; font-size: 9.6pt; line-height: 1.42; color: #1d1f21; }
h1 { font-size: 21pt; color: #12594f; border-bottom: 2px solid #1a8a7a; padding-bottom: 4px; margin-top: 0; }
h2 { font-size: 14pt; color: #12594f; border-bottom: 1px solid #c8d6d3; padding-bottom: 2px; margin-top: 22px;
     break-after: avoid; }
h2:not(:first-of-type) { break-before: page; }
h3 { font-size: 11pt; color: #1d1f21; margin-top: 14px; break-after: avoid; }
p, li { orphans: 3; widows: 3; }
a { color: #0f6e62; text-decoration: none; word-break: break-word; }
code { font-family: Consolas, "Courier New", monospace; font-size: 8.6pt; background: #eef2f1; padding: 0 2px;
       border-radius: 2px; }
pre { background: #f2f5f4; border: 1px solid #d8e0de; border-radius: 3px; padding: 6px 8px; white-space: pre-wrap;
      break-inside: avoid; }
pre code { background: none; padding: 0; }
table { border-collapse: collapse; width: 100%; margin: 6px 0 10px 0; font-size: 8.8pt; }
th, td { border: 1px solid #c9d3d1; padding: 3px 5px; vertical-align: top; text-align: left; }
th { background: #e3eeeb; }
tr { break-inside: avoid; }
img { max-width: 100%; border: 1px solid #b9c3c1; margin: 4px 0; }
hr { border: none; border-top: 1px solid #c8d6d3; }
.cover { break-after: page; text-align: center; padding-top: 62mm; }
.cover img.logo { width: 150mm; max-width: 100%; border: none; margin: 0 auto 18mm auto; display: block; }
.cover .title { font-size: 28pt; font-weight: 600; color: #12594f; margin: 0 0 6mm 0; }
.cover .subtitle { font-size: 12pt; color: #3a3f42; margin: 0 0 30mm 0; }
.cover .small { font-size: 9.5pt; color: #5b6366; margin: 2mm 0; }
"""


def slug(text):
    """GitHub-style heading anchor: lower case, punctuation dropped, spaces to hyphens."""
    s = text.strip().lower()
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")


def to_html(md_text, title, subtitle, version):
    md = MarkdownIt("commonmark", {"html": False}).enable("table")
    tokens = md.parse(md_text)
    # the cover page carries the title and the subtitle under the logo: the document's own first heading goes
    if tokens and tokens[0].type == "heading_open" and tokens[0].tag == "h1":
        tokens = tokens[3:]
    for i, t in enumerate(tokens):
        if t.type == "heading_open":
            t.attrSet("id", slug(tokens[i + 1].content))
    body = md.renderer.render(tokens, md.options, {})
    cover = (f'<div class="cover"><img class="logo" src="{LOGO}" alt="DJAP - DJ Audio Processor">'
             f'<p class="title">{html.escape(title)}</p><p class="subtitle">{html.escape(subtitle)}</p>'
             + (f'<p class="small">Version {html.escape(version)}</p>' if version else "")
             + '<p class="small">https://github.com/hghaken/DJAP</p>'
             '<p class="small">Free software under the GNU General Public License v3</p></div>')
    return (f"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>{html.escape(title)}</title>"
            f"<style>{CSS}</style></head><body>{cover}{body}</body></html>")


def browser():
    for p in (r"C:\Program Files\Google\Chrome\Application\chrome.exe",
              r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
              r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
              r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"):
        if os.path.exists(p):
            return p
    sys.exit("no Chrome or Edge found for printing the PDF")


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else ""
    exe = browser()
    profile = tempfile.mkdtemp(prefix="djap-pdf-")
    try:
        for src, pdf, title, subtitle in MANUALS:
            with open(os.path.join(DOCS, src), encoding="utf-8") as f:
                page = to_html(f.read(), title, subtitle, version)
            # next to the markdown, so the relative image paths resolve; removed afterwards
            tmp_html = os.path.join(DOCS, "_print_" + os.path.splitext(src)[0] + ".html")
            with open(tmp_html, "w", encoding="utf-8") as f:
                f.write(page)
            out = os.path.join(DOCS, pdf)
            try:
                subprocess.run([exe, "--headless=new", "--disable-gpu", "--no-first-run", "--no-pdf-header-footer",
                                f"--user-data-dir={profile}", f"--print-to-pdf={out}",
                                "file:///" + tmp_html.replace("\\", "/")],
                               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
            finally:
                os.remove(tmp_html)
            print(f"{out} ({os.path.getsize(out) / 1024:.0f} kB)")
    finally:
        shutil.rmtree(profile, ignore_errors=True)


if __name__ == "__main__":
    main()
