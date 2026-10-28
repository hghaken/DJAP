// The update check's pure part (docs/design.md section 8, "Updates"): reading GitHub's "latest release" answer,
// comparing versions, and SHA-256 to check a downloaded setup against the digest GitHub publishes for it. No
// network and no Windows code here (src/app/updater.* does the downloads).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ap {

struct ReleaseInfo {
    std::string version;   // "0.1.95" (the tag without its leading v)
    std::string page;      // the release's web page
    std::string setupName; // "DJAP-Setup-0.1.95.exe"
    std::string setupUrl;  // where to download it
    uint64_t setupSize = 0;
    std::string sha256;    // lower-case hex from the asset's digest ("sha256:..."); empty when GitHub gave none
    std::string sha256Url; // a "<setup>.sha256" asset, when there is one (used when the digest is missing)
};

// Reads GitHub's answer to GET /repos/<owner>/<repo>/releases/latest. False (and why) when it is not valid JSON or
// has no tag or no setup ("DJAP-Setup-*.exe") among its assets.
bool parseLatestRelease(const std::string& json, ReleaseInfo& out, std::string& error);

// The release from where GitHub's web page of the latest release leads ("https://github.com/<owner>/<repo>/releases/
// tag/v0.1.95"): the version, and the setup and its .sha256 at their fixed download addresses (no size). For when
// GitHub's API refuses (403 / 429: its limit of 60 questions an hour per network without a login is used up - the
// web pages have no such limit). False when the address is not a release tag "v<version>" of exactly this repository
// (`repo`: "https://github.com/<owner>/<repo>"; seventh outside review: any owner / repo was taken).
bool releaseFromTagUrl(const std::string& url, const std::string& repo, ReleaseInfo& out);

// Compares two versions part by part as numbers ("0.1.10" > "0.1.9"); a leading "v" and anything from "+" or "-" on
// ("+dev") are ignored, missing parts count as 0. <0, 0 or >0.
int compareVersions(const std::string& a, const std::string& b);

// The checksum of `setupName` in a .sha256 file: the 64-hex word on the line that names the setup (as sha256sum
// writes "<hash>  <name>"), or the file's only 64-hex word. Lower case; empty when it is not clear which one it is.
std::string checksumFor(const std::string& text, const std::string& setupName);

// SHA-256 (FIPS 180-4), fed in pieces.
class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t n);
    std::string hex(); // finishes; lower-case hex of the 32-byte digest
private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t bytes_ = 0;
};

} // namespace ap
