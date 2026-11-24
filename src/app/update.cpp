// SPDX-License-Identifier: MIT
// One-click updates from GitHub Releases (p1e2r/tributary): check the latest release,
// download the build for this OS and CPU, unpack it with our own gzip/tar/zip reader,
// swap it in place of the running copy, and restart.
#include <filesystem>
#include <thread>

#include "app.hpp"
#include "../git/archive.hpp"
#include "../net/http.hpp"

namespace fs = std::filesystem;

namespace app {

using gfx::Align;
using gfx::FontId;

static const char* kReleasesApi = "https://api.github.com/repos/p1e2r/tributary/releases/latest";

static std::vector<int> version_parts(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    v = v.substr(0, v.find_first_of("-+ "));
    std::vector<int> out;
    size_t i = 0;
    while (i < v.size()) {
        size_t j = v.find('.', i);
        if (j == std::string::npos) j = v.size();
        try { out.push_back(std::stoi(v.substr(i, j - i))); } catch (...) { out.push_back(0); }
        i = j + 1;
    }
    while (out.size() < 3) out.push_back(0);
    return out;
}

static bool newer(const std::string& a, const std::string& b) { return version_parts(a) > version_parts(b); }

// Which release asset is ours.
static bool asset_matches(const std::string& name) {
    std::string n = git::to_lower(name), os = plat::os_name(), arch = plat::arch_name();
    if (os == "Windows") return n.find("windows") != std::string::npos && n.size() > 4 && n.substr(n.size() - 4) == ".zip" &&
                                (n.find("arm64") == std::string::npos) == (arch != "arm64");
    if (os == "macOS") return n.find("macos-" + arch) != std::string::npos && n.size() > 4 && n.substr(n.size() - 4) == ".zip";
    return n.find("linux-" + arch) != std::string::npos && n.size() > 7 && n.substr(n.size() - 7) == ".tar.gz";
}

bool App::update_banner_visible() const {
    using U = UpdateState;
    if (update.dismissed) return false;
    return update.s == U::Available || update.s == U::Downloading || update.s == U::Installing || (update.s == U::Failed && !update.asset_url.empty());
}

void App::check_for_updates(bool manual) {
    using U = UpdateState;
    if (update.s == U::Checking || update.s == U::Downloading || update.s == U::Installing) return;
    update.s = U::Checking;
    update.manual = manual;
    update.last_check = ui.time;
    if (manual) update.dismissed = false;
    std::string api = kReleasesApi;
    if (const char* e = std::getenv("TRIBUTARY_UPDATE_API"); e && *e) api = e;
    std::string token = account.valid() && !account.enterprise() ? account.token : "";
    auto alive = alive_;
    std::thread([this, api, token, alive] {
        UpdateState u;
        u.s = U::UpToDate;
        try {
            net::Request rq;
            rq.url = api;
            rq.headers["Accept"] = "application/vnd.github+json";
            if (!token.empty()) rq.headers["Authorization"] = "Bearer " + token;
            auto r = net::request(rq);
            if (r.status == 404) throw std::runtime_error("No releases have been published yet.");
            if (r.status >= 400) throw std::runtime_error("GitHub answered " + std::to_string(r.status) + " when checking for updates.");
            auto v = json::parse(r.body);
            std::string tag = v["tag_name"].str();
            u.version = tag.empty() || (tag[0] != 'v' && tag[0] != 'V') ? tag : tag.substr(1);
            u.notes_url = v["html_url"].str();
            if (!v["draft"].boolean(false) && !v["prerelease"].boolean(false) && newer(u.version, TRIBUTARY_VERSION)) {
                u.s = U::Available;
                for (auto& a : v["assets"].arr())
                    if (asset_matches(a["name"].str())) {
                        u.asset_name = a["name"].str();
                        u.asset_url = a["browser_download_url"].str();
                        u.size = size_t(a["size"].integer());
                        break;
                    }
                if (u.asset_url.empty()) {
                    u.s = U::Failed;
                    u.error = "Version " + u.version + " has no download for " + plat::os_name() + " (" + plat::arch_name() + ").";
                }
            }
        } catch (const std::exception& e) {
            u.s = U::Failed;
            u.error = e.what();
        }
        if (!alive->load()) return;
        jobs.post([this, u] {
            bool manual = update.manual;
            double lc = update.last_check;
            update.s = u.s;
            update.version = u.version;
            update.notes_url = u.notes_url;
            update.asset_url = u.asset_url;
            update.asset_name = u.asset_name;
            update.size = u.size;
            update.error = u.error;
            update.last_check = lc;
            // quiet background checks: say nothing unless there's something to install
            if (!manual && u.s == UpdateState::Failed && u.asset_url.empty()) update.s = UpdateState::Idle;
            if (manual && u.s == UpdateState::UpToDate) toast("You're on the latest version (" + std::string(TRIBUTARY_VERSION) + ").");
        });
    }).detach();
}

// ---------------------------------------------------------------- install
namespace {

struct Plan {
    fs::path install_root;  // what gets replaced: the .app bundle, or the folder holding the binary
    fs::path relaunch;      // what to start afterwards
    bool bundle = false;
};

Plan make_plan() {
    Plan p;
    fs::path exe = git::git_to_path(plat::executable_path());
    if (exe.empty()) throw std::runtime_error("Couldn't find where Tributary is installed.");
#ifdef __APPLE__
    // …/Tributary.app/Contents/MacOS/Tributary
    fs::path app = exe.parent_path().parent_path().parent_path();
    if (app.extension() == ".app") {
        p.install_root = app;
        p.relaunch = app;
        p.bundle = true;
        return p;
    }
#endif
    p.install_root = exe.parent_path();
    p.relaunch = exe;
    return p;
}

fs::path find_named(const fs::path& dir, const std::string& name, bool want_dir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->path().filename() == name && (want_dir ? it->is_directory() : it->is_regular_file())) return it->path();
    }
    return {};
}

std::string u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Move every file under `from` into `to`, replacing what's there. A running executable can't be
// overwritten on Windows but can be renamed, so old files are moved aside as *.old first.
void merge_tree(const fs::path& from, const fs::path& to) {
    std::vector<fs::path> files;
    for (auto& e : fs::recursive_directory_iterator(from))
        if (!e.is_directory()) files.push_back(e.path());
    for (auto& f : files) {
        fs::path dst = to / fs::relative(f, from);
        fs::create_directories(dst.parent_path());
        std::error_code ec;
#ifdef _WIN32
        if (fs::exists(dst)) {
            fs::path old = dst;
            old += ".old";
            fs::remove(old, ec);
            fs::rename(dst, old);
        }
#endif
        fs::rename(f, dst);
    }
}

}  // namespace

// Leftovers from the previous update (called at startup).
void cleanup_after_update() {
    try {
        auto plan = make_plan();
        fs::path parent = plan.bundle ? plan.install_root.parent_path() : plan.install_root;
        std::error_code ec;
        fs::remove_all(parent / ".tributary-update", ec);
#ifdef _WIN32
        for (auto it = fs::recursive_directory_iterator(plan.install_root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->path().extension() == ".old") fs::remove(it->path(), ec);
        }
#endif
    } catch (...) {}
}

void App::install_update() {
    using U = UpdateState;
    if (update.s != U::Available && update.s != U::Failed) return;
    if (update.asset_url.empty()) { if (!update.notes_url.empty()) plat::open_url(update.notes_url); return; }
    update.s = U::Downloading;
    update.got->store(0);
    std::string url = update.asset_url, name = update.asset_name;
    auto got = update.got;
    auto alive = alive_;
    std::thread([this, url, name, got, alive] {
        std::string err;
        Plan plan;
        try {
            plan = make_plan();
            net::Request rq;
            rq.url = url;
            rq.headers["Accept"] = "application/octet-stream";
            auto r = net::request(rq, [got](size_t n) { got->store(n); });
            if (r.status >= 400) throw std::runtime_error("The download failed (HTTP " + std::to_string(r.status) + ").");
            if (alive->load()) jobs.post([this] { update.s = UpdateState::Installing; });
            fs::path parent = plan.bundle ? plan.install_root.parent_path() : plan.install_root;
            fs::path stage = parent / ".tributary-update";
            std::error_code ec;
            fs::remove_all(stage, ec);
            fs::create_directories(stage, ec);
            if (ec) throw std::runtime_error("Tributary can't write to " + u8(parent) + ". Move it to a folder you own, or install the update from the release page.");
            git::extract_archive(r.body, stage / "new");
            if (plan.bundle) {
                fs::path app = find_named(stage / "new", plan.install_root.filename().string(), true);
                if (app.empty()) app = find_named(stage / "new", "Tributary.app", true);
                if (app.empty()) throw std::runtime_error("The download doesn't contain Tributary.app.");
                fs::path old = stage / "previous.app";
                fs::rename(plan.install_root, old);
                try {
                    fs::rename(app, plan.install_root);
                } catch (...) {
                    fs::rename(old, plan.install_root);  // put the old one back
                    throw;
                }
                fs::remove_all(stage, ec);
            } else {
                std::string exe_name = plan.relaunch.filename().string();
                fs::path bin = find_named(stage / "new", exe_name, false);
                if (bin.empty()) bin = find_named(stage / "new", plat::os_name() == "Windows" ? "Tributary.exe" : "Tributary", false);
                if (bin.empty()) throw std::runtime_error("The download doesn't contain " + exe_name + ".");
                merge_tree(bin.parent_path(), plan.install_root);
                fs::remove_all(stage, ec);
#ifndef _WIN32
                fs::permissions(plan.relaunch, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec, ec);
#endif
            }
        } catch (const std::exception& e) {
            err = e.what();
        }
        if (!alive->load()) return;
        jobs.post([this, err, plan] {
            if (!err.empty()) {
                update.s = UpdateState::Failed;
                update.error = "Couldn't install the update: " + err;
                return;
            }
            save_state();
#ifdef __APPLE__
            if (plan.bundle) plat::spawn_detached({"/usr/bin/open", "-n", u8(plan.relaunch)});
            else plat::spawn_detached({u8(plan.relaunch)});
#else
            plat::spawn_detached({u8(plan.relaunch)});
#endif
            should_quit = true;
        });
    }).detach();
}

void App::draw_update_banner(const Rect& rc) {
    using U = UpdateState;
    auto& T = ui.theme;
    r.rect(rc, T.accent_subtle);
    r.line_h(rc.x, rc.right(), rc.bottom() - 1, T.border);
    bool failed = update.s == U::Failed;
    r.icon(failed ? "alert" : "desktop-download", rc.x + 14, rc.y + (rc.h - 16) / 2, 16, failed ? T.danger : T.accent);
    float right = rc.right() - 40;
    if (update.s == U::Available || failed) {
        const char* label = failed ? "Try again" : "Update and restart";
        float bw = ui.text_w(FontId::Regular, 12, label) + 28;
        if (ui::button(ui, ui.id("upd-install"), {right - bw, rc.y + 7, bw, rc.h - 14}, label, ui::BtnStyle::Primary)) {
            if (failed) { update.s = U::Available; }
            install_update();
        }
        right -= bw + 8;
        if (!update.notes_url.empty()) {
            float lw = ui.text_w(FontId::Regular, 12, "What's new") + 16;
            if (ui::button(ui, ui.id("upd-notes"), {right - lw, rc.y + 7, lw, rc.h - 14}, "What's new", ui::BtnStyle::Link)) plat::open_url(update.notes_url);
            right -= lw + 8;
        }
    } else {
        // progress bar
        float bw = 180;
        Rect bar{right - bw, rc.y + rc.h / 2 - 3, bw, 6};
        r.rect(bar, T.border, 3);
        float f = update.s == U::Installing ? 1.f : update.size ? std::min(1.f, float(update.got->load()) / float(update.size)) : 0.f;
        r.rect({bar.x, bar.y, std::max(6.f, bar.w * f), bar.h}, T.accent_emphasis, 3);
        right -= bw + 12;
        ui.request_redraw();
    }
    std::string msg;
    switch (update.s) {
        case U::Available: msg = "Tributary " + update.version + " is available (you have " + TRIBUTARY_VERSION + ")."; break;
        case U::Downloading: {
            int pct = update.size ? int(100.0 * double(update.got->load()) / double(update.size)) : 0;
            msg = "Downloading Tributary " + update.version + "… " + std::to_string(std::min(pct, 100)) + "%";
            break;
        }
        case U::Installing: msg = "Installing Tributary " + update.version + "… it will restart in a moment."; break;
        default: msg = update.error; break;
    }
    float tw = right - (rc.x + 40) - 8;
    r.draw_text_in(FontId::Regular, 12, {rc.x + 40, rc.y, tw, rc.h}, r.ellipsize(FontId::Regular, 12, msg, tw), failed ? T.danger : T.text);
    bool busy = update.s == U::Downloading || update.s == U::Installing;
    if (!busy && ui::icon_button(ui, ui.id("upd-close"), {rc.right() - 34, rc.y + 8, 24, rc.h - 16}, "x", "Dismiss")) update.dismissed = true;
}

}  // namespace app
