// SPDX-License-Identifier: MIT
// Windows platform layer (Win32 API).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <iterator>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shobjidl.h>

#include <deque>
#include <stdexcept>

#include "platform.hpp"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")

namespace plat {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string narrow(const wchar_t* w, int len = -1) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    if (len < 0 && !s.empty()) s.pop_back();  // drop NUL
    return s;
}

Key map_vk(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return Key(int(Key::A) + int(vk - 'A'));
    if (vk >= '0' && vk <= '9') return Key(int(Key::Num0) + int(vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F12) return Key(int(Key::F1) + int(vk - VK_F1));
    switch (vk) {
        case VK_RETURN: return Key::Enter;
        case VK_ESCAPE: return Key::Escape;
        case VK_BACK: return Key::Backspace;
        case VK_TAB: return Key::Tab;
        case VK_SPACE: return Key::Space;
        case VK_DELETE: return Key::Delete;
        case VK_INSERT: return Key::Insert;
        case VK_HOME: return Key::Home;
        case VK_END: return Key::End;
        case VK_PRIOR: return Key::PageUp;
        case VK_NEXT: return Key::PageDown;
        case VK_LEFT: return Key::Left;
        case VK_RIGHT: return Key::Right;
        case VK_UP: return Key::Up;
        case VK_DOWN: return Key::Down;
        case VK_OEM_COMMA: return Key::Comma;
        case VK_OEM_PERIOD: return Key::Period;
        case VK_OEM_2: return Key::Slash;
        case VK_OEM_MINUS: return Key::Minus;
        case VK_OEM_PLUS: return Key::Equals;
    }
    return Key::Unknown;
}

uint8_t cur_mods() {
    uint8_t m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= ModShift;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= ModCtrl;
    if (GetKeyState(VK_MENU) & 0x8000) m |= ModAlt;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= ModSuper;
    return m;
}

class Win32Window : public Window {
public:
    Win32Window(const std::string& title, int w, int h) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof wc};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Win32Window::proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = L"TributaryWindow";
        wc.hbrBackground = CreateSolidBrush(RGB(13, 17, 23));
        RegisterClassExW(&wc);
        // size in pixels for the monitor DPI
        UINT dpi = GetDpiForSystem();
        scale_ = float(dpi) / 96.f;
        RECT r{0, 0, LONG(w * scale_), LONG(h * scale_)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, widen(title).c_str(), WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, this);
        if (!hwnd_) throw std::runtime_error("CreateWindowEx failed");
        scale_ = float(GetDpiForWindow(hwnd_)) / 96.f;
        BOOL dark = is_dark_mode() ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
        ShowWindow(hwnd_, SW_SHOWDEFAULT);
        UpdateWindow(hwnd_);
        RECT cr;
        GetClientRect(hwnd_, &cr);
        width_ = cr.right;
        height_ = cr.bottom;
        cursors_[0] = LoadCursorW(nullptr, IDC_ARROW);
        cursors_[1] = LoadCursorW(nullptr, IDC_IBEAM);
        cursors_[2] = LoadCursorW(nullptr, IDC_HAND);
        cursors_[3] = LoadCursorW(nullptr, IDC_SIZEWE);
        cursors_[4] = LoadCursorW(nullptr, IDC_SIZENS);
        cursors_[5] = LoadCursorW(nullptr, IDC_WAIT);
    }

    ~Win32Window() override {
        if (hwnd_) DestroyWindow(hwnd_);
        CoUninitialize();
    }

    void wait_events(int timeout_ms) override {
        if (!queue_.empty()) { pump(); return; }
        MsgWaitForMultipleObjectsEx(0, nullptr, timeout_ms < 0 ? INFINITE : DWORD(timeout_ms), QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
        pump();
    }

    bool poll(Event& e) override {
        pump();
        if (queue_.empty()) return false;
        e = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void wake() override { PostMessageW(hwnd_, WM_APP + 1, 0, 0); }
    int pixel_width() const override { return width_; }
    int pixel_height() const override { return height_; }
    float scale() const override { return scale_; }
    void set_title(const std::string& t) override { SetWindowTextW(hwnd_, widen(t).c_str()); }
    void set_cursor(Cursor c) override {
        cursor_ = int(c);
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd_, &p);
        RECT cr;
        GetClientRect(hwnd_, &cr);
        if (PtInRect(&cr, p)) SetCursor(cursors_[cursor_]);
    }

    std::string clipboard_get() override {
        std::string s;
        if (!OpenClipboard(hwnd_)) return s;
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
                s = narrow(w);
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        // normalize CRLF
        std::string out;
        for (size_t i = 0; i < s.size(); i++) if (!(s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n')) out += s[i];
        return out;
    }

    void clipboard_set(const std::string& s) override {
        std::wstring w = widen(s);
        if (!OpenClipboard(hwnd_)) return;
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
        if (g) {
            memcpy(GlobalLock(g), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(g);
            SetClipboardData(CF_UNICODETEXT, g);
        }
        CloseClipboard();
    }

    std::vector<const char*> vk_instance_extensions() const override { return {"VK_KHR_surface", "VK_KHR_win32_surface"}; }

    VkSurfaceKHR vk_create_surface(VkInstance inst, PFN_vkGetInstanceProcAddr gipa) override {
        auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(gipa(inst, "vkCreateWin32SurfaceKHR"));
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        ci.hinstance = GetModuleHandleW(nullptr);
        ci.hwnd = hwnd_;
        VkSurfaceKHR s = VK_NULL_HANDLE;
        if (!create || create(inst, &ci, nullptr, &s) != VK_SUCCESS) throw std::runtime_error("vkCreateWin32SurfaceKHR failed");
        return s;
    }

    std::optional<std::string> pick_folder(const std::string& title, const std::string& start) override {
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(widen(title).c_str());
        IShellItem* folder = nullptr;
        if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(widen(start).c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
        std::optional<std::string> result;
        if (SUCCEEDED(dlg->Show(hwnd_))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = narrow(path);
                    for (auto& ch : *result) if (ch == '\\') ch = '/';
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
        return result;
    }

    bool is_dark_mode() const override {
        DWORD v = 1, sz = sizeof v;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
            return v == 0;
        return false;
    }

private:
    void pump() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { Event e; e.type = Event::Close; queue_.push_back(e); continue; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void push(Event e) { queue_.push_back(std::move(e)); }

    void mouse(Event::Type t, LPARAM lp, int button) {
        Event e;
        e.type = t;
        e.x = float(GET_X_LPARAM(lp)) / scale_;
        e.y = float(GET_Y_LPARAM(lp)) / scale_;
        e.button = button;
        e.mods = cur_mods();
        if (t == Event::MouseDown) {
            DWORD now = GetMessageTime();
            bool near_ = std::abs(e.x - lx_) < 4 && std::abs(e.y - ly_) < 4;
            clicks_ = (DWORD(now - last_click_) < GetDoubleClickTime() && near_ && button == last_btn_) ? clicks_ + 1 : 1;
            last_click_ = now;
            lx_ = e.x;
            ly_ = e.y;
            last_btn_ = button;
            e.clicks = clicks_;
            SetCapture(hwnd_);
        } else if (t == Event::MouseUp) {
            ReleaseCapture();
        }
        push(e);
    }

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
        Win32Window* self;
        if (m == WM_NCCREATE) {
            self = static_cast<Win32Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(self));
            self->hwnd_ = h;
        } else {
            self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(h, m, wp, lp);
        return self->handle(m, wp, lp);
    }

    LRESULT handle(UINT m, WPARAM wp, LPARAM lp) {
        Event e;
        switch (m) {
            case WM_CLOSE: e.type = Event::Close; push(e); return 0;
            case WM_SIZE:
                width_ = LOWORD(lp);
                height_ = HIWORD(lp);
                e.type = Event::Resize;
                push(e);
                return 0;
            case WM_DPICHANGED: {
                scale_ = float(HIWORD(wp)) / 96.f;
                auto* r = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                e.type = Event::Resize;
                push(e);
                return 0;
            }
            case WM_GETMINMAXINFO: {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                mm->ptMinTrackSize.x = LONG(760 * scale_);
                mm->ptMinTrackSize.y = LONG(480 * scale_);
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(hwnd_, &ps);
                EndPaint(hwnd_, &ps);
                e.type = Event::Resize;
                push(e);
                return 0;
            }
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) { SetCursor(cursors_[cursor_]); return TRUE; }
                break;
            case WM_MOUSEMOVE: {
                if (!tracking_) {
                    TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
                    TrackMouseEvent(&t);
                    tracking_ = true;
                }
                mouse(Event::MouseMove, lp, 0);
                return 0;
            }
            case WM_MOUSELEAVE:
                tracking_ = false;
                e.type = Event::MouseMove;
                e.x = e.y = -1000;
                push(e);
                return 0;
            case WM_LBUTTONDOWN: mouse(Event::MouseDown, lp, 0); return 0;
            case WM_LBUTTONUP: mouse(Event::MouseUp, lp, 0); return 0;
            case WM_RBUTTONDOWN: mouse(Event::MouseDown, lp, 1); return 0;
            case WM_RBUTTONUP: mouse(Event::MouseUp, lp, 1); return 0;
            case WM_MBUTTONDOWN: mouse(Event::MouseDown, lp, 2); return 0;
            case WM_MBUTTONUP: mouse(Event::MouseUp, lp, 2); return 0;
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL: {
                POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd_, &p);
                e.type = Event::Scroll;
                e.x = float(p.x) / scale_;
                e.y = float(p.y) / scale_;
                float d = float(GET_WHEEL_DELTA_WPARAM(wp)) / float(WHEEL_DELTA) * 48.f;
                e.mods = cur_mods();
                if (m == WM_MOUSEWHEEL) {
                    if (e.mods & ModShift) e.dx = d; else e.dy = d;
                } else e.dx = -d;
                push(e);
                return 0;
            }
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP: {
                bool down = m == WM_KEYDOWN || m == WM_SYSKEYDOWN;
                e.type = down ? Event::KeyDown : Event::KeyUp;
                e.key = map_vk(wp);
                e.mods = cur_mods();
                e.repeat = down && (lp & (1 << 30));
                push(e);
                if (m == WM_SYSKEYDOWN && wp == VK_F4) break;  // let Alt+F4 close
                if (m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) return 0;
                return 0;
            }
            case WM_CHAR: {
                wchar_t c = wchar_t(wp);
                if (c >= 0xD800 && c <= 0xDBFF) { high_surrogate_ = c; return 0; }
                std::wstring w;
                if (c >= 0xDC00 && c <= 0xDFFF && high_surrogate_) { w += high_surrogate_; w += c; high_surrogate_ = 0; }
                else w += c;
                if (c < 32 || c == 127) return 0;
                if (cur_mods() & ModCtrl && !(cur_mods() & ModAlt)) return 0;  // Ctrl shortcuts aren't text (AltGr is)
                e.type = Event::Text;
                e.text = narrow(w.c_str(), int(w.size()));
                push(e);
                return 0;
            }
            case WM_SETFOCUS: e.type = Event::FocusGained; push(e); return 0;
            case WM_KILLFOCUS: e.type = Event::FocusLost; push(e); return 0;
            case WM_DROPFILES: {
                HDROP d = HDROP(wp);
                wchar_t buf[MAX_PATH * 4];
                UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; i++) {
                    DragQueryFileW(d, i, buf, UINT(std::size(buf)));
                    Event de;
                    de.type = Event::Drop;
                    de.text = narrow(buf);
                    for (auto& ch : de.text) if (ch == '\\') ch = '/';
                    push(de);
                }
                DragFinish(d);
                return 0;
            }
            case WM_SETTINGCHANGE: {
                BOOL dark = is_dark_mode() ? TRUE : FALSE;
                DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof dark);
                break;
            }
            case WM_APP + 1: e.type = Event::Wake; push(e); return 0;
        }
        return DefWindowProcW(hwnd_, m, wp, lp);
    }

    HWND hwnd_ = nullptr;
    int width_ = 0, height_ = 0;
    float scale_ = 1;
    std::deque<Event> queue_;
    HCURSOR cursors_[6]{};
    int cursor_ = 0;
    bool tracking_ = false;
    DWORD last_click_ = 0;
    float lx_ = 0, ly_ = 0;
    int clicks_ = 0, last_btn_ = -1;
    wchar_t high_surrogate_ = 0;
};

}  // namespace

std::unique_ptr<Window> Window::create(const std::string& title, int w, int h) {
    return std::make_unique<Win32Window>(title, w, h);
}

}  // namespace plat
