// SPDX-License-Identifier: MIT
// Tributary entry point: window + Vulkan renderer + event loop.
#include <cstdio>
#include <cstring>
#include <fstream>

#include "app.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

static void write_png_free_ppm(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
    std::ofstream f(path, std::ios::binary);
    f << "P6 " << w << " " << h << " 255\n";
    for (int i = 0; i < w * h; i++) f.write(reinterpret_cast<const char*>(&rgba[size_t(i) * 4]), 3);
}

static int run(int argc, char** argv) {
    std::string screenshot;
    double screenshot_after = 1.5;
    std::vector<std::string> open_paths;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else if (a == "--screenshot-delay" && i + 1 < argc) screenshot_after = std::atof(argv[++i]);
        else if (a == "--version") { std::puts("Tributary 0.1.0"); return 0; }
        else if (a.size() && a[0] != '-') open_paths.push_back(a);
    }
    auto win = plat::Window::create("Tributary", 1200, 780);
    gfx::Renderer renderer(*win);
    app::App application(*win, renderer);
    for (auto& p : open_paths) application.add_repo_path(p, true);

    double start = plat::now_seconds();
    bool redraw = true;
    while (!application.should_quit) {
        bool animating = application.ui.animating() || application.jobs.busy();
        win->wait_events(redraw ? 0 : animating ? 16 : 500);
        plat::Event e;
        bool had = false;
        while (win->poll(e)) {
            application.on_event(e);
            had = true;
        }
        redraw = false;
        application.frame();
        if (application.ui.wants_redraw()) redraw = true;
        (void)had;
        if (!screenshot.empty() && plat::now_seconds() - start > screenshot_after) {
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            if (renderer.capture(px, w, h)) {
                write_png_free_ppm(screenshot, px, w, h);
                break;
            }
            redraw = true;
        }
    }
    return 0;
}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n), 0);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
        s.resize(strlen(s.c_str()));
        args.push_back(s);
    }
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    try {
        return run(int(argv.size()), argv.data());
    } catch (const std::exception& ex) {
        int n = MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, nullptr, 0);
        std::wstring w(size_t(n), 0);
        MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, w.data(), n);
        MessageBoxW(nullptr, w.c_str(), L"Tributary couldn't start", MB_ICONERROR);
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Tributary couldn't start: %s\n", ex.what());
        return 1;
    }
}
#endif
