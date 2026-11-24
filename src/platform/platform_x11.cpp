// SPDX-License-Identifier: MIT
// Linux/BSD platform layer over Xlib (works under XWayland too).
#include "platform.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

#include <vulkan/vulkan_xlib.h>

namespace plat {
namespace {

Key map_key(KeySym ks) {
    if (ks >= XK_a && ks <= XK_z) return Key(int(Key::A) + int(ks - XK_a));
    if (ks >= XK_A && ks <= XK_Z) return Key(int(Key::A) + int(ks - XK_A));
    if (ks >= XK_0 && ks <= XK_9) return Key(int(Key::Num0) + int(ks - XK_0));
    if (ks >= XK_F1 && ks <= XK_F12) return Key(int(Key::F1) + int(ks - XK_F1));
    switch (ks) {
        case XK_Return: case XK_KP_Enter: return Key::Enter;
        case XK_Escape: return Key::Escape;
        case XK_BackSpace: return Key::Backspace;
        case XK_Tab: case XK_ISO_Left_Tab: return Key::Tab;
        case XK_space: return Key::Space;
        case XK_Delete: case XK_KP_Delete: return Key::Delete;
        case XK_Insert: return Key::Insert;
        case XK_Home: case XK_KP_Home: return Key::Home;
        case XK_End: case XK_KP_End: return Key::End;
        case XK_Page_Up: return Key::PageUp;
        case XK_Page_Down: return Key::PageDown;
        case XK_Left: case XK_KP_Left: return Key::Left;
        case XK_Right: case XK_KP_Right: return Key::Right;
        case XK_Up: case XK_KP_Up: return Key::Up;
        case XK_Down: case XK_KP_Down: return Key::Down;
        case XK_comma: return Key::Comma;
        case XK_period: return Key::Period;
        case XK_slash: return Key::Slash;
        case XK_minus: return Key::Minus;
        case XK_equal: return Key::Equals;
    }
    return Key::Unknown;
}

uint8_t map_mods(unsigned state) {
    uint8_t m = 0;
    if (state & ShiftMask) m |= ModShift;
    if (state & ControlMask) m |= ModCtrl;
    if (state & Mod1Mask) m |= ModAlt;
    if (state & Mod4Mask) m |= ModSuper;
    return m;
}

class X11Window : public Window {
public:
    X11Window(const std::string& title, int w, int h) {
        XInitThreads();
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) throw std::runtime_error("cannot open X display (is DISPLAY set?)");
        scale_ = detect_scale();
        int screen = DefaultScreen(dpy_);
        XSetWindowAttributes swa{};
        swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                         PointerMotionMask | StructureNotifyMask | FocusChangeMask | LeaveWindowMask;
        swa.background_pixel = 0x1f2328;
        win_ = XCreateWindow(dpy_, RootWindow(dpy_, screen), 0, 0, unsigned(w * scale_), unsigned(h * scale_), 0,
                             CopyFromParent, InputOutput, CopyFromParent, CWEventMask | CWBackPixel, &swa);
        wm_delete_ = XInternAtom(dpy_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(dpy_, win_, &wm_delete_, 1);
        clipboard_ = XInternAtom(dpy_, "CLIPBOARD", False);
        utf8_ = XInternAtom(dpy_, "UTF8_STRING", False);
        targets_ = XInternAtom(dpy_, "TARGETS", False);
        prop_ = XInternAtom(dpy_, "TRIBUTARY_SEL", False);
        XSizeHints* hints = XAllocSizeHints();
        hints->flags = PMinSize;
        hints->min_width = int(760 * scale_);
        hints->min_height = int(480 * scale_);
        XSetWMNormalHints(dpy_, win_, hints);
        XFree(hints);
        XClassHint ch{const_cast<char*>("tributary"), const_cast<char*>("Tributary")};
        XSetClassHint(dpy_, win_, &ch);
        set_title(title);
        XMapWindow(dpy_, win_);
        im_ = XOpenIM(dpy_, nullptr, nullptr, nullptr);
        if (im_)
            ic_ = XCreateIC(im_, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win_,
                            XNFocusWindow, win_, nullptr);
        if (pipe(wake_pipe_) == 0) {
            fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);
            fcntl(wake_pipe_[1], F_SETFL, O_NONBLOCK);
        }
        width_ = int(w * scale_);
        height_ = int(h * scale_);
        cursors_[0] = XCreateFontCursor(dpy_, XC_left_ptr);
        cursors_[1] = XCreateFontCursor(dpy_, XC_xterm);
        cursors_[2] = XCreateFontCursor(dpy_, XC_hand2);
        cursors_[3] = XCreateFontCursor(dpy_, XC_sb_h_double_arrow);
        cursors_[4] = XCreateFontCursor(dpy_, XC_sb_v_double_arrow);
        cursors_[5] = XCreateFontCursor(dpy_, XC_watch);
        XFlush(dpy_);
    }

    ~X11Window() override {
        if (ic_) XDestroyIC(ic_);
        if (im_) XCloseIM(im_);
        XDestroyWindow(dpy_, win_);
        XCloseDisplay(dpy_);
    }

    float detect_scale() {
        if (auto env = std::getenv("TRIBUTARY_SCALE")) return float(std::atof(env));
        XrmInitialize();
        char* rms = XResourceManagerString(dpy_);
        if (rms) {
            XrmDatabase db = XrmGetStringDatabase(rms);
            XrmValue v;
            char* type = nullptr;
            if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &v) && v.addr) {
                float dpi = float(std::atof(v.addr));
                XrmDestroyDatabase(db);
                if (dpi > 0) return dpi / 96.0f;
            }
            XrmDestroyDatabase(db);
        }
        return 1.0f;
    }

    void wait_events(int timeout_ms) override {
        if (!queue_.empty() || XPending(dpy_)) { pump(); return; }
        int fd = ConnectionNumber(dpy_);
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        FD_SET(wake_pipe_[0], &fds);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        select(std::max(fd, wake_pipe_[0]) + 1, &fds, nullptr, nullptr, timeout_ms < 0 ? nullptr : &tv);
        char buf[64];
        bool woke = false;
        while (read(wake_pipe_[0], buf, sizeof buf) > 0) woke = true;
        if (woke) { Event e; e.type = Event::Wake; queue_.push_back(e); }
        pump();
    }

    bool poll(Event& e) override {
        pump();
        if (queue_.empty()) return false;
        e = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void wake() override {
        char c = 1;
        [[maybe_unused]] auto r = write(wake_pipe_[1], &c, 1);
    }

    int pixel_width() const override { return width_; }
    int pixel_height() const override { return height_; }
    float scale() const override { return scale_; }

    void set_title(const std::string& t) override {
        XStoreName(dpy_, win_, t.c_str());
        Atom net_name = XInternAtom(dpy_, "_NET_WM_NAME", False);
        XChangeProperty(dpy_, win_, net_name, utf8_, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(t.data()), int(t.size()));
    }

    void set_cursor(Cursor c) override {
        int i = int(c);
        if (i == cur_cursor_) return;
        cur_cursor_ = i;
        XDefineCursor(dpy_, win_, cursors_[i]);
        XFlush(dpy_);
    }

    std::string clipboard_get() override {
        ::Window owner = XGetSelectionOwner(dpy_, clipboard_);
        if (owner == win_) return clip_;
        if (owner == None) return "";
        XConvertSelection(dpy_, clipboard_, utf8_, prop_, win_, CurrentTime);
        XFlush(dpy_);
        double deadline = now_seconds() + 1.0;
        while (now_seconds() < deadline) {
            XEvent ev;
            if (XCheckTypedWindowEvent(dpy_, win_, SelectionNotify, &ev)) {
                if (ev.xselection.property == None) return "";
                Atom type;
                int fmt;
                unsigned long n, after;
                unsigned char* data = nullptr;
                XGetWindowProperty(dpy_, win_, prop_, 0, 1 << 24, True, AnyPropertyType, &type, &fmt, &n, &after, &data);
                std::string s = data ? std::string(reinterpret_cast<char*>(data), n) : "";
                if (data) XFree(data);
                return s;
            }
            usleep(2000);
        }
        return "";
    }

    void clipboard_set(const std::string& s) override {
        clip_ = s;
        XSetSelectionOwner(dpy_, clipboard_, win_, CurrentTime);
        XFlush(dpy_);
    }

    std::vector<const char*> vk_instance_extensions() const override {
        return {"VK_KHR_surface", "VK_KHR_xlib_surface"};
    }

    VkSurfaceKHR vk_create_surface(VkInstance inst, PFN_vkGetInstanceProcAddr gipa) override {
        auto create = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(gipa(inst, "vkCreateXlibSurfaceKHR"));
        VkXlibSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
        ci.dpy = dpy_;
        ci.window = win_;
        VkSurfaceKHR s = VK_NULL_HANDLE;
        if (!create || create(inst, &ci, nullptr, &s) != VK_SUCCESS) throw std::runtime_error("vkCreateXlibSurfaceKHR failed");
        return s;
    }

    std::optional<std::string> pick_folder(const std::string& title, const std::string& start) override {
        for (auto tool : {"zenity", "kdialog"}) {
            std::string probe = std::string("command -v ") + tool + " >/dev/null 2>&1";
            if (std::system(probe.c_str()) != 0) continue;
            std::string cmd = std::string(tool) == "zenity"
                                  ? "zenity --file-selection --directory --title=\"" + title + "\" --filename=\"" + start + "/\" 2>/dev/null"
                                  : "kdialog --getexistingdirectory \"" + start + "\" --title \"" + title + "\" 2>/dev/null";
            FILE* p = popen(cmd.c_str(), "r");
            if (!p) continue;
            char buf[4096];
            std::string out;
            while (fgets(buf, sizeof buf, p)) out += buf;
            pclose(p);
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            if (out.empty()) return std::nullopt;
            return out;
        }
        return std::nullopt;
    }

private:
    void push(Event e) { queue_.push_back(std::move(e)); }

    void pump() {
        while (XPending(dpy_)) {
            XEvent ev;
            XNextEvent(dpy_, &ev);
            if (XFilterEvent(&ev, None)) continue;
            Event e;
            switch (ev.type) {
                case ConfigureNotify:
                    if (ev.xconfigure.width != width_ || ev.xconfigure.height != height_) {
                        width_ = ev.xconfigure.width;
                        height_ = ev.xconfigure.height;
                        e.type = Event::Resize;
                        push(e);
                    }
                    break;
                case Expose:
                    e.type = Event::Resize;  // force a redraw
                    push(e);
                    break;
                case ClientMessage:
                    if (Atom(ev.xclient.data.l[0]) == wm_delete_) { e.type = Event::Close; push(e); }
                    break;
                case MotionNotify:
                    e.type = Event::MouseMove;
                    e.x = float(ev.xmotion.x) / scale_;
                    e.y = float(ev.xmotion.y) / scale_;
                    e.mods = map_mods(ev.xmotion.state);
                    push(e);
                    break;
                case ButtonPress:
                case ButtonRelease: {
                    unsigned b = ev.xbutton.button;
                    e.x = float(ev.xbutton.x) / scale_;
                    e.y = float(ev.xbutton.y) / scale_;
                    e.mods = map_mods(ev.xbutton.state);
                    if (b >= 4 && b <= 7) {
                        if (ev.type == ButtonRelease) break;
                        e.type = Event::Scroll;
                        if (b == 4) e.dy = 1; else if (b == 5) e.dy = -1;
                        else if (b == 6) e.dx = 1; else e.dx = -1;
                        e.dx *= 48; e.dy *= 48;
                        if (e.mods & ModShift) std::swap(e.dx, e.dy);
                        push(e);
                        break;
                    }
                    e.type = ev.type == ButtonPress ? Event::MouseDown : Event::MouseUp;
                    e.button = b == 1 ? 0 : b == 3 ? 1 : 2;
                    if (ev.type == ButtonPress) {
                        double t = double(ev.xbutton.time) / 1000.0;
                        bool near = std::abs(e.x - last_click_x_) < 4 && std::abs(e.y - last_click_y_) < 4;
                        click_count_ = (t - last_click_t_ < 0.4 && near && e.button == last_btn_) ? click_count_ + 1 : 1;
                        last_click_t_ = t; last_click_x_ = e.x; last_click_y_ = e.y; last_btn_ = e.button;
                        e.clicks = click_count_;
                    }
                    push(e);
                    break;
                }
                case KeyPress:
                case KeyRelease: {
                    KeySym ks = 0;
                    char buf[64];
                    int n = 0;
                    Status st = 0;
                    if (ev.type == KeyPress && ic_) n = Xutf8LookupString(ic_, &ev.xkey, buf, sizeof buf - 1, &ks, &st);
                    else n = XLookupString(&ev.xkey, buf, sizeof buf - 1, &ks, nullptr);
                    if (ev.type == KeyPress && ic_ && (st == XLookupKeySym || st == XLookupNone)) n = 0;
                    KeySym base = XLookupKeysym(&ev.xkey, 0);
                    e.type = ev.type == KeyPress ? Event::KeyDown : Event::KeyUp;
                    e.key = map_key(base ? base : ks);
                    e.mods = map_mods(ev.xkey.state);
                    // detect auto-repeat (release immediately followed by press)
                    if (ev.type == KeyRelease && XEventsQueued(dpy_, QueuedAfterReading)) {
                        XEvent next;
                        XPeekEvent(dpy_, &next);
                        if (next.type == KeyPress && next.xkey.time == ev.xkey.time && next.xkey.keycode == ev.xkey.keycode)
                            break;  // drop the synthetic release
                    }
                    push(e);
                    if (ev.type == KeyPress && n > 0 && !(e.mods & (ModCtrl | ModSuper))) {
                        buf[n] = 0;
                        unsigned char c0 = (unsigned char)buf[0];
                        if (c0 >= 32 && c0 != 127) {
                            Event t;
                            t.type = Event::Text;
                            t.text.assign(buf, size_t(n));
                            push(t);
                        }
                    }
                    break;
                }
                case FocusIn: e.type = Event::FocusGained; push(e); if (ic_) XSetICFocus(ic_); break;
                case FocusOut: e.type = Event::FocusLost; push(e); if (ic_) XUnsetICFocus(ic_); break;
                case LeaveNotify:
                    e.type = Event::MouseMove;
                    e.x = -1000; e.y = -1000;
                    push(e);
                    break;
                case SelectionRequest: {
                    auto& rq = ev.xselectionrequest;
                    XSelectionEvent se{};
                    se.type = SelectionNotify;
                    se.requestor = rq.requestor;
                    se.selection = rq.selection;
                    se.target = rq.target;
                    se.time = rq.time;
                    se.property = None;
                    if (rq.target == targets_) {
                        Atom list[] = {targets_, utf8_, XA_STRING};
                        XChangeProperty(dpy_, rq.requestor, rq.property, XA_ATOM, 32, PropModeReplace,
                                        reinterpret_cast<unsigned char*>(list), 3);
                        se.property = rq.property;
                    } else if (rq.target == utf8_ || rq.target == XA_STRING) {
                        XChangeProperty(dpy_, rq.requestor, rq.property, rq.target, 8, PropModeReplace,
                                        reinterpret_cast<const unsigned char*>(clip_.data()), int(clip_.size()));
                        se.property = rq.property;
                    }
                    XSendEvent(dpy_, rq.requestor, True, 0, reinterpret_cast<XEvent*>(&se));
                    XFlush(dpy_);
                    break;
                }
                default: break;
            }
        }
    }

    Display* dpy_ = nullptr;
    ::Window win_ = 0;
    Atom wm_delete_, clipboard_, utf8_, targets_, prop_;
    XIM im_ = nullptr;
    XIC ic_ = nullptr;
    int wake_pipe_[2]{-1, -1};
    int width_ = 0, height_ = 0;
    float scale_ = 1;
    std::deque<Event> queue_;
    std::string clip_;
    ::Cursor cursors_[6];
    int cur_cursor_ = -1;
    double last_click_t_ = 0;
    float last_click_x_ = 0, last_click_y_ = 0;
    int click_count_ = 0, last_btn_ = -1;
};

}  // namespace

std::unique_ptr<Window> Window::create(const std::string& title, int w, int h) {
    return std::make_unique<X11Window>(title, w, h);
}

}  // namespace plat
