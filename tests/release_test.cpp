// The update check's pure part: SHA-256 against the standard's vectors, reading a GitHub release answer, version
// order, and answers that must be refused.
#include "check.h"
#include "release.h"

#include <string>

using namespace ap;

namespace {

std::string sha(const std::string& s) {
    Sha256 h;
    h.update(s.data(), s.size());
    return h.hex();
}

void sha256Vectors() {
    std::printf("-- SHA-256 (FIPS 180-4 examples)\n");
    CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // a million 'a', fed in uneven pieces (the block boundaries fall inside the pieces)
    Sha256 h;
    const std::string piece(997, 'a');
    size_t left = 1000000;
    while (left > 0) {
        const size_t n = left < piece.size() ? left : piece.size();
        h.update(piece.data(), n);
        left -= n;
    }
    CHECK(h.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void versions() {
    std::printf("-- version order\n");
    CHECK(compareVersions("0.1.95", "0.1.93") > 0);
    CHECK(compareVersions("0.1.10", "0.1.9") > 0);   // numbers, not text
    CHECK(compareVersions("v0.1.93", "0.1.93") == 0); // the tag's v
    CHECK(compareVersions("0.1.93+dev", "0.1.93") == 0);
    CHECK(compareVersions("0.2", "0.1.99") > 0);
    CHECK(compareVersions("1.0", "1.0.0") == 0);      // a missing part is 0
    CHECK(compareVersions("0.1.93", "0.1.94") < 0);
}

const char* kRelease = R"({
  "url": "https://api.github.com/repos/owner/DJAP/releases/1",
  "html_url": "https://github.com/owner/DJAP/releases/tag/v0.1.95",
  "tag_name": "v0.1.95",
  "name": "DJAP 0.1.95 \u2013 \"quoted\" \ud83c\udfb5",
  "draft": false, "prerelease": false, "id": 123456789,
  "assets": [
    {"name": "DJAP-Setup-0.1.95.exe.sha256", "size": 87,
     "browser_download_url": "https://github.com/owner/DJAP/releases/download/v0.1.95/DJAP-Setup-0.1.95.exe.sha256"},
    {"name": "DJAP-Setup-0.1.95.exe", "size": 6132669, "content_type": "application/x-msdownload",
     "digest": "sha256:0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef",
     "browser_download_url": "https://github.com/owner/DJAP/releases/download/v0.1.95/DJAP-Setup-0.1.95.exe",
     "uploader": {"login": "owner", "site_admin": false}, "download_count": 0, "label": null}
  ],
  "body": "line 1\nline 2\ttab \\ backslash"
})";

void release() {
    std::printf("-- a GitHub release answer\n");
    ReleaseInfo r;
    std::string err;
    CHECK(parseLatestRelease(kRelease, r, err));
    std::printf("   version %s, setup %s (%llu bytes), sha256 %s, page %s\n", r.version.c_str(), r.setupName.c_str(),
                static_cast<unsigned long long>(r.setupSize), r.sha256.c_str(), r.page.c_str());
    CHECK(r.version == "0.1.95");
    CHECK(r.setupName == "DJAP-Setup-0.1.95.exe");
    CHECK(r.setupUrl == "https://github.com/owner/DJAP/releases/download/v0.1.95/DJAP-Setup-0.1.95.exe");
    CHECK(r.setupSize == 6132669);
    CHECK(r.sha256 == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"); // lower case
    CHECK(r.sha256Url == "https://github.com/owner/DJAP/releases/download/v0.1.95/DJAP-Setup-0.1.95.exe.sha256");
    CHECK(r.page == "https://github.com/owner/DJAP/releases/tag/v0.1.95");

    std::printf("-- answers that are refused\n");
    const char* bad[] = {
        "",                                                       // empty
        "{",                                                      // cut off
        "{\"tag_name\": \"v1\"} trailing",                        // text after the value
        "[1, 2]",                                                 // not an object
        "{\"message\": \"Not Found\"}",                           // GitHub's 404 body: no tag
        "{\"tag_name\": \"v0.1.95\", \"assets\": []}",            // no setup
        "{\"tag_name\": \"v0.1.95\", \"assets\": [{\"name\": \"notes.txt\", \"browser_download_url\": \"x\"}]}",
        "{\"tag_name\": \"v0.1.95\" \"html_url\": \"x\"}",        // a missing comma
        "{\"tag_name\": \"\\ud83c\"}",                            // half a surrogate pair
    };
    int refused = 0;
    for (const char* j : bad) {
        ReleaseInfo x;
        std::string e;
        if (!parseLatestRelease(j, x, e)) ++refused;
        else std::printf("   accepted by mistake: %s\n", j);
    }
    std::printf("   %d of %d refused\n", refused, static_cast<int>(sizeof bad / sizeof bad[0]));
    CHECK(refused == static_cast<int>(sizeof bad / sizeof bad[0]));

    // a digest that is not SHA-256 hex is not taken (the .sha256 asset is used then)
    ReleaseInfo y;
    CHECK(parseLatestRelease("{\"tag_name\": \"0.1.96\", \"assets\": [{\"name\": \"DJAP-Setup-0.1.96.exe\", "
                             "\"digest\": \"sha512:abc\", \"browser_download_url\": \"u\"}]}",
                             y, err));
    CHECK(y.version == "0.1.96" && y.sha256.empty() && y.setupUrl == "u");
}

// The .sha256 file: the hash on the setup's own line, or the only hash; never a guess between several.
void checksumFile() {
    const std::string a(64, 'a'), b(64, 'b');
    CHECK(checksumFor(a + "  DJAP-Setup-0.1.96.exe\n", "DJAP-Setup-0.1.96.exe") == a);
    CHECK(checksumFor(a + " *DJAP-Setup-0.1.96.exe\r\n", "DJAP-Setup-0.1.96.exe") == a); // binary-mode marker
    CHECK(checksumFor(std::string(64, 'A'), "x.exe") == a);                               // the only one, upper case
    CHECK(checksumFor(b + "  other.exe\n" + a + "  DJAP-Setup-0.1.96.exe\n", "DJAP-Setup-0.1.96.exe") == a);
    CHECK(checksumFor(b + "  other.exe\n" + a + "  more.exe\n", "DJAP-Setup-0.1.96.exe").empty()); // which one?
    CHECK(checksumFor("no hash here", "DJAP-Setup-0.1.96.exe").empty());
    CHECK(checksumFor(a + "a  DJAP-Setup-0.1.96.exe", "DJAP-Setup-0.1.96.exe").empty()); // 65 characters
}

} // namespace

int main() {
    sha256Vectors();
    versions();
    release();
    checksumFile();
    return test::finish("release_test");
}
