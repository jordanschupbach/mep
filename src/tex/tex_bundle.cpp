#include "tex/tex_bundle.h"

#include "deflate.h"
#include "pdf_crypt.h"

#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace meptex {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[] = "MEPTEXB1\n";

bool IsFile(const std::string &p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

std::string Hex(const std::string &bytes) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (char ch : bytes) {
        const auto b = static_cast<unsigned char>(ch);
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 15]);
    }
    return out;
}

uint32_t Le32(const std::string &d, size_t i) {
    return static_cast<uint32_t>(static_cast<unsigned char>(d[i])) |
           static_cast<uint32_t>(static_cast<unsigned char>(d[i + 1])) << 8 |
           static_cast<uint32_t>(static_cast<unsigned char>(d[i + 2])) << 16 |
           static_cast<uint32_t>(static_cast<unsigned char>(d[i + 3])) << 24;
}

// A path inside the bundle that stays inside the tree it is unpacked to.
bool SafePath(const std::string &p) {
    if (p.empty() || p[0] == '/' || p.find('\\') != std::string::npos) return false;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string part = p.substr(i, j - i);
        if (part.empty() || part == "." || part == "..") return false;
        i = j + 1;
    }
    return true;
}

}  // namespace

std::string FindTexBundle() {
    if (const char *e = std::getenv("MEP_TEX_BUNDLE"); e && *e) return IsFile(e) ? e : "";
    std::error_code ec;
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    const fs::path exe = _NSGetExecutablePath(buf, &size) == 0 ? fs::path(buf) : fs::path();
    if (exe.empty()) ec = std::make_error_code(std::errc::no_such_file_or_directory);
#else
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
#endif
    if (!ec) {
        const fs::path dir = exe.parent_path();
        for (const fs::path &p : {dir.parent_path() / "share/mep/tex/tex-bundle.mtb", dir / "tex-bundle.mtb"}) {
            if (IsFile(p.string())) return p.string();
        }
    }
    return "";
}

std::string UnpackTexBundle(const std::string &bundle, const std::string &cache_dir, bool verbose, std::string *err) {
    // One directory per bundle file as it is now (path, size, time), so a
    // compile checks for it without reading the bundle; complete once
    // `.complete` is in it.
    std::error_code ec0;
    const auto size0 = fs::file_size(bundle, ec0);
    const auto time0 = fs::last_write_time(bundle, ec0);
    if (ec0) {
        *err = "cannot read the TeX bundle " + bundle;
        return "";
    }
    const std::string key = Hex(pdfcrypt::Md5(fs::absolute(bundle).string() + ":" + std::to_string(size0) + ":" +
                                              std::to_string(time0.time_since_epoch().count())))
                                .substr(0, 16);
    const std::string dir = cache_dir + "/bundle-" + key;
    if (IsFile(dir + "/.complete")) return dir;

    if (verbose) std::fprintf(stderr, "mep-tex: unpacking the TeX bundle into %s (once)\n", dir.c_str());
    std::ifstream in(bundle, std::ios::binary);
    if (!in) {
        *err = "cannot read the TeX bundle " + bundle;
        return "";
    }
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t magic_len = sizeof kMagic - 1;
    if (data.size() < magic_len + 4 || data.compare(0, magic_len, kMagic) != 0) {
        *err = bundle + " is not a TeX bundle";
        return "";
    }
    std::error_code ec;
    const std::string tmp = dir + ".tmp" + std::to_string(::getpid());
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) {
        *err = "cannot make " + tmp + ": " + ec.message();
        return "";
    }
    const uint32_t count = Le32(data, magic_len);
    size_t i = magic_len + 4;
    std::map<std::string, std::vector<std::string>> listing;  // directory -> its files, for ls-R
    for (uint32_t k = 0; k < count; k++) {
        if (i + 2 > data.size()) break;
        const size_t name_len = static_cast<size_t>(static_cast<unsigned char>(data[i])) |
                                static_cast<size_t>(static_cast<unsigned char>(data[i + 1])) << 8;
        i += 2;
        if (i + name_len + 13 > data.size()) break;
        const std::string name = data.substr(i, name_len);
        i += name_len;
        const int method = static_cast<unsigned char>(data[i]);
        const uint32_t size = Le32(data, i + 1), stored = Le32(data, i + 5), crc = Le32(data, i + 9);
        i += 13;
        if (i + stored > data.size() || !SafePath(name)) {
            *err = bundle + " is damaged (at " + name + ")";
            fs::remove_all(tmp, ec);
            return "";
        }
        std::string body;
        if (method == 1) {
            if (!deflate::InflateZlib(reinterpret_cast<const unsigned char *>(data.data() + i), stored, body)) body.clear();
        } else {
            body = data.substr(i, stored);
        }
        i += stored;
        if (body.size() != size ||
            deflate::Crc32(0, reinterpret_cast<const unsigned char *>(body.data()), body.size()) != crc) {
            *err = bundle + " is damaged (" + name + " does not check out)";
            fs::remove_all(tmp, ec);
            return "";
        }
        const fs::path out = fs::path(tmp) / name;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary);
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!f) {
            *err = "cannot write " + out.string();
            fs::remove_all(tmp, ec);
            return "";
        }
        const size_t slash = name.rfind('/');
        listing[slash == std::string::npos ? "" : name.substr(0, slash)].push_back(
            slash == std::string::npos ? name : name.substr(slash + 1));
    }
    // ls-R, kpathsea's file list, so the tree is indexed without a walk
    {
        std::ofstream ls(fs::path(tmp) / "ls-R");
        ls << "% ls-R -- filename database for kpathsea; do not change this line.\n";
        for (const auto &[d, names] : listing) {
            ls << "\n./" << d << ":\n";
            for (const std::string &n : names) ls << n << "\n";
        }
    }
    std::ofstream(fs::path(tmp) / ".complete") << bundle << "\n";
    fs::rename(tmp, dir, ec);
    if (ec) {
        fs::remove_all(tmp, ec);  // unpacked meanwhile by another run
        if (!IsFile(dir + "/.complete")) {
            *err = "cannot store the TeX bundle in " + dir;
            return "";
        }
        return dir;
    }
    // Trees of older bundles are dead weight.
    std::error_code ec2;
    for (fs::directory_iterator it(cache_dir, ec2), end; !ec2 && it != end; it.increment(ec2)) {
        const std::string n = it->path().filename().string();
        if (n.rfind("bundle-", 0) == 0 && it->path() != fs::path(dir) && n.find(".tmp") == std::string::npos)
            fs::remove_all(it->path(), ec2);
    }
    return dir;
}

}  // namespace meptex
