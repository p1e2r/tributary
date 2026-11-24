// SPDX-License-Identifier: MIT
// macOS platform layer in pure C++: Cocoa is driven through the Objective-C
// runtime's C API (objc_msgSend, objc_allocateClassPair, ...), so there is no
// Objective-C source in the project. Rendering goes through a CAMetalLayer and
// MoltenVK (VK_EXT_metal_surface).
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <atomic>
#include <deque>
#include <stdexcept>

#include "platform.hpp"

namespace plat {
namespace {

using id = ::id;

// ---------------------------------------------------------------- runtime helpers
inline SEL S(const char* s) { return sel_registerName(s); }
inline Class C(const char* s) { return objc_getClass(s); }

template <typename R = id, typename... A>
inline R msg(id obj, const char* sel, A... a) {
    return reinterpret_cast<R (*)(id, SEL, A...)>(objc_msgSend)(obj, S(sel), a...);
}
template <typename R = id, typename... A>
inline R msgc(Class cls, const char* sel, A... a) {
    return reinterpret_cast<R (*)(id, SEL, A...)>(objc_msgSend)(reinterpret_cast<id>(cls), S(sel), a...);
}
// Struct-returning messages (NSRect is 32 bytes: x86_64 needs the _stret variant).
inline CGRect msg_rect(id obj, const char* sel) {
#if defined(__x86_64__)
    CGRect r;
    reinterpret_cast<void (*)(CGRect*, id, SEL)>(objc_msgSend_stret)(&r, obj, S(sel));
    return r;
#else
    return reinterpret_cast<CGRect (*)(id, SEL)>(objc_msgSend)(obj, S(sel));
#endif
}
inline CGPoint msg_point(id obj, const char* sel) { return reinterpret_cast<CGPoint (*)(id, SEL)>(objc_msgSend)(obj, S(sel)); }

id nsstr(const std::string& s) { return msgc(C("NSString"), "stringWithUTF8String:", s.c_str()); }
std::string from_ns(id s) {
    if (!s) return "";
    const char* c = msg<const char*>(s, "UTF8String");
    return c ? c : "";
}

constexpr unsigned long kShift = 1ul << 17, kCtrl = 1ul << 18, kAlt = 1ul << 19, kCmd = 1ul << 20;

uint8_t mods_of(id ev) {
    unsigned long f = msg<unsigned long>(ev, "modifierFlags");
    uint8_t m = 0;
    if (f & kShift) m |= ModShift;
    if (f & kCtrl) m |= ModCtrl;
    if (f & kAlt) m |= ModAlt;
    if (f & kCmd) m |= ModSuper;
    return m;
}

Key map_keycode(unsigned short k) {
    // kVK_* virtual key codes (Carbon HIToolbox/Events.h)
    static const struct { unsigned short code; Key key; } table[] = {
        {0x00, Key::A}, {0x0B, Key::B}, {0x08, Key::C}, {0x02, Key::D}, {0x0E, Key::E}, {0x03, Key::F}, {0x05, Key::G},
        {0x04, Key::H}, {0x22, Key::I}, {0x26, Key::J}, {0x28, Key::K}, {0x25, Key::L}, {0x2E, Key::M}, {0x2D, Key::N},
        {0x1F, Key::O}, {0x23, Key::P}, {0x0C, Key::Q}, {0x0F, Key::R}, {0x01, Key::S}, {0x11, Key::T}, {0x20, Key::U},
        {0x09, Key::V}, {0x0D, Key::W}, {0x07, Key::X}, {0x10, Key::Y}, {0x06, Key::Z},
        {0x1D, Key::Num0}, {0x12, Key::Num1}, {0x13, Key::Num2}, {0x14, Key::Num3}, {0x15, Key::Num4}, {0x17, Key::Num5},
        {0x16, Key::Num6}, {0x1A, Key::Num7}, {0x1C, Key::Num8}, {0x19, Key::Num9},
        {0x24, Key::Enter}, {0x4C, Key::Enter}, {0x35, Key::Escape}, {0x33, Key::Backspace}, {0x30, Key::Tab},
        {0x31, Key::Space}, {0x75, Key::Delete}, {0x73, Key::Home}, {0x77, Key::End}, {0x74, Key::PageUp},
        {0x79, Key::PageDown}, {0x7B, Key::Left}, {0x7C, Key::Right}, {0x7E, Key::Up}, {0x7D, Key::Down},
        {0x7A, Key::F1}, {0x78, Key::F2}, {0x63, Key::F3}, {0x76, Key::F4}, {0x60, Key::F5}, {0x61, Key::F6},
        {0x62, Key::F7}, {0x64, Key::F8}, {0x65, Key::F9}, {0x6D, Key::F10}, {0x67, Key::F11}, {0x6F, Key::F12},
        {0x2B, Key::Comma}, {0x2F, Key::Period}, {0x2C, Key::Slash}, {0x1B, Key::Minus}, {0x18, Key::Equals},
    };
    for (auto& t : table) if (t.code == k) return t.key;
    return Key::Unknown;
}

class CocoaWindow;
CocoaWindow* g_win = nullptr;  // single-window app

class CocoaWindow : public Window {
public:
    CocoaWindow(const std::string& title, int w, int h);

    void wait_events(int timeout_ms) override {
        id pool = msg(msgc(C("NSAutoreleasePool"), "alloc"), "init");
        id until = timeout_ms < 0 ? msgc(C("NSDate"), "distantFuture")
                                  : msgc(C("NSDate"), "dateWithTimeIntervalSinceNow:", double(timeout_ms) / 1000.0);
        if (queue_.empty()) dispatch_one(until);
        // drain everything else that is already pending
        while (dispatch_one(msgc(C("NSDate"), "distantPast"))) {}
        msg<void>(pool, "drain");
    }

    bool poll(Event& e) override {
        if (queue_.empty()) {
            id pool = msg(msgc(C("NSAutoreleasePool"), "alloc"), "init");
            while (dispatch_one(msgc(C("NSDate"), "distantPast"))) {}
            msg<void>(pool, "drain");
        }
        if (queue_.empty()) return false;
        e = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void wake() override {
        // Posting an application-defined event is safe from any thread.
        id pool = msg(msgc(C("NSAutoreleasePool"), "alloc"), "init");
        id ev = msgc(C("NSEvent"), "otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:",
                     (unsigned long)15 /*NSEventTypeApplicationDefined*/, CGPoint{0, 0}, (unsigned long)0, 0.0, (long)0,
                     (id) nullptr, (short)0, (long)0, (long)0);
        msg<void>(msgc(C("NSApplication"), "sharedApplication"), "postEvent:atStart:", ev, (signed char)0);
        msg<void>(pool, "drain");
    }

    int pixel_width() const override { return pw_; }
    int pixel_height() const override { return ph_; }
    float scale() const override { return scale_; }
    void set_title(const std::string& t) override { msg<void>(window_, "setTitle:", nsstr(t)); }

    void set_cursor(Cursor c) override {
        if (int(c) == cursor_) return;
        cursor_ = int(c);
        const char* names[] = {"arrowCursor", "IBeamCursor", "pointingHandCursor", "resizeLeftRightCursor",
                               "resizeUpDownCursor", "arrowCursor"};
        msg<void>(msgc(C("NSCursor"), names[cursor_]), "set");
    }

    std::string clipboard_get() override {
        id pb = msgc(C("NSPasteboard"), "generalPasteboard");
        return from_ns(msg(pb, "stringForType:", nsstr("public.utf8-plain-text")));
    }
    void clipboard_set(const std::string& s) override {
        id pb = msgc(C("NSPasteboard"), "generalPasteboard");
        msg<long>(pb, "clearContents");
        msg<signed char>(pb, "setString:forType:", nsstr(s), nsstr("public.utf8-plain-text"));
    }

    std::vector<const char*> vk_instance_extensions() const override { return {"VK_KHR_surface", "VK_EXT_metal_surface"}; }

    VkSurfaceKHR vk_create_surface(VkInstance inst, PFN_vkGetInstanceProcAddr gipa) override {
        struct MetalCI {
            VkStructureType sType;
            const void* pNext;
            VkFlags flags;
            const void* pLayer;
        } ci{VkStructureType(1000217000) /*VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT*/, nullptr, 0, layer_};
        using Fn = VkResult (*)(VkInstance, const MetalCI*, const VkAllocationCallbacks*, VkSurfaceKHR*);
        auto create = reinterpret_cast<Fn>(gipa(inst, "vkCreateMetalSurfaceEXT"));
        VkSurfaceKHR s = VK_NULL_HANDLE;
        if (!create || create(inst, &ci, nullptr, &s) != VK_SUCCESS) throw std::runtime_error("vkCreateMetalSurfaceEXT failed (is MoltenVK installed?)");
        return s;
    }

    std::optional<std::string> pick_folder(const std::string& title, const std::string& start) override {
        id panel = msgc(C("NSOpenPanel"), "openPanel");
        msg<void>(panel, "setCanChooseDirectories:", (signed char)1);
        msg<void>(panel, "setCanChooseFiles:", (signed char)0);
        msg<void>(panel, "setAllowsMultipleSelection:", (signed char)0);
        msg<void>(panel, "setCanCreateDirectories:", (signed char)1);
        msg<void>(panel, "setMessage:", nsstr(title));
        if (!start.empty()) msg<void>(panel, "setDirectoryURL:", msgc(C("NSURL"), "fileURLWithPath:", nsstr(start)));
        long r = msg<long>(panel, "runModal");
        if (r != 1) return std::nullopt;
        id urls = msg(panel, "URLs");
        id url = msg(urls, "firstObject");
        if (!url) return std::nullopt;
        return from_ns(msg(url, "path"));
    }

    bool is_dark_mode() const override {
        id app = msgc(C("NSApplication"), "sharedApplication");
        id appearance = msg(app, "effectiveAppearance");
        if (!appearance) return false;
        id names = nullptr;
        id aqua = nsstr("NSAppearanceNameAqua"), dark = nsstr("NSAppearanceNameDarkAqua");
        id objs[2] = {aqua, dark};
        names = msgc(C("NSArray"), "arrayWithObjects:count:", objs, (unsigned long)2);
        id best = msg(appearance, "bestMatchFromAppearancesWithNames:", names);
        return best && msg<signed char>(best, "isEqualToString:", dark);
    }

    // called from the runtime-created classes
    void push(Event e) { queue_.push_back(std::move(e)); }
    void update_size() {
        CGRect b = msg_rect(view_, "bounds");
        scale_ = float(msg<double>(window_, "backingScaleFactor"));
        if (scale_ <= 0) scale_ = 1;
        pw_ = int(b.size.width * scale_);
        ph_ = int(b.size.height * scale_);
        msg<void>(layer_, "setContentsScale:", double(scale_));
        msg<void>(layer_, "setDrawableSize:", CGSize{double(pw_), double(ph_)});
    }
    void mouse_event(id ev, Event::Type t, int button) {
        CGPoint p = reinterpret_cast<CGPoint (*)(id, SEL)>(objc_msgSend)(ev, S("locationInWindow"));
        CGRect b = msg_rect(view_, "bounds");
        Event e;
        e.type = t;
        e.x = float(p.x);
        e.y = float(b.size.height - p.y);
        e.button = button;
        e.mods = mods_of(ev);
        if (t == Event::MouseDown) e.clicks = int(msg<long>(ev, "clickCount"));
        push(e);
    }
    id view_ = nullptr, window_ = nullptr, layer_ = nullptr;

    bool has_native_menu() const override { return true; }
    float titlebar_height() const override { return 22; }
    void set_menu(const std::vector<MenuSpec>& menus) override;

private:
    bool dispatch_one(id until) {
        id app = msgc(C("NSApplication"), "sharedApplication");
        id ev = msg(app, "nextEventMatchingMask:untilDate:inMode:dequeue:", ~0ull, until,
                    nsstr("kCFRunLoopDefaultMode"), (signed char)1);
        if (!ev) return false;
        if (msg<unsigned long>(ev, "type") == 15) {
            Event e;
            e.type = Event::Wake;
            push(e);
            return true;
        }
        msg<void>(app, "sendEvent:", ev);
        return true;
    }

    std::deque<Event> queue_;
    int pw_ = 0, ph_ = 0;
    float scale_ = 2;
    int cursor_ = -1;
};

// ---------------------------------------------------------------- runtime classes
signed char yes_imp(id, SEL) { return 1; }
signed char yes1_imp(id, SEL, id) { return 1; }

void key_down(id, SEL, id ev) {
    if (!g_win) return;
    Event e;
    e.type = Event::KeyDown;
    e.key = map_keycode(msg<unsigned short>(ev, "keyCode"));
    e.mods = mods_of(ev);
    e.repeat = msg<signed char>(ev, "isARepeat");
    g_win->push(e);
    if (!(e.mods & (ModSuper | ModCtrl))) {
        std::string chars = from_ns(msg(ev, "characters"));
        // drop control characters and the private-use range used for function keys
        std::string out;
        for (size_t i = 0; i < chars.size();) {
            unsigned char c = (unsigned char)chars[i];
            size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            uint32_t cp = c;
            if (n == 3) cp = ((c & 0x0F) << 12) | ((chars[i + 1] & 0x3F) << 6) | (chars[i + 2] & 0x3F);
            bool skip = (cp < 32 || cp == 127) || (cp >= 0xF700 && cp <= 0xF8FF);
            if (!skip) out.append(chars, i, n);
            i += n;
        }
        if (!out.empty()) {
            Event t;
            t.type = Event::Text;
            t.text = out;
            g_win->push(t);
        }
    }
}
void key_up(id, SEL, id ev) {
    if (!g_win) return;
    Event e;
    e.type = Event::KeyUp;
    e.key = map_keycode(msg<unsigned short>(ev, "keyCode"));
    e.mods = mods_of(ev);
    g_win->push(e);
}
// Cmd+key combos arrive here first; claim them so the app sees them as key events.
signed char perform_key_equivalent(id self, SEL, id ev) {
    unsigned long f = msg<unsigned long>(ev, "modifierFlags");
    if (!(f & kCmd)) return 0;
    unsigned short code = msg<unsigned short>(ev, "keyCode");
    if (code == 0x0C /*Q*/ || code == 0x04 /*H*/) return 0;  // leave Quit/Hide to the menu
    key_down(self, S("keyDown:"), ev);
    return 1;
}
void flags_changed(id, SEL, id) {}
void m_down(id, SEL, id ev) {
    if (!g_win) return;
    // the title bar strip drags the window, like a native title bar
    CGPoint p = reinterpret_cast<CGPoint (*)(id, SEL)>(objc_msgSend)(ev, S("locationInWindow"));
    CGRect b = msg_rect(g_win->view_, "bounds");
    float y = float(b.size.height - p.y);
    if (y < g_win->titlebar_height() && p.x > 76) {
        if (msg<long>(ev, "clickCount") == 2) msg<void>(g_win->window_, "zoom:", (id) nullptr);
        else msg<void>(g_win->window_, "performWindowDragWithEvent:", ev);
        return;
    }
    g_win->mouse_event(ev, Event::MouseDown, 0);
}
void menu_action(id, SEL, id sender) {
    if (!g_win) return;
    Event e;
    e.type = Event::Command;
    e.command = int(msg<long>(sender, "tag"));
    g_win->push(e);
}
Class menu_target_class() {
    static Class cls = [] {
        Class c = objc_allocateClassPair(C("NSObject"), "TributaryMenuTarget", 0);
        class_addMethod(c, S("menuAction:"), (IMP)menu_action, "v@:@");
        objc_registerClassPair(c);
        return c;
    }();
    return cls;
}
void m_up(id, SEL, id ev) { if (g_win) g_win->mouse_event(ev, Event::MouseUp, 0); }
void rm_down(id, SEL, id ev) { if (g_win) g_win->mouse_event(ev, Event::MouseDown, 1); }
void rm_up(id, SEL, id ev) { if (g_win) g_win->mouse_event(ev, Event::MouseUp, 1); }
void om_down(id, SEL, id ev) { if (g_win) g_win->mouse_event(ev, Event::MouseDown, 2); }
void om_up(id, SEL, id ev) { if (g_win) g_win->mouse_event(ev, Event::MouseUp, 2); }
void m_move(id, SEL, id ev) { if (g_win) g_win->mouse_event(ev, Event::MouseMove, 0); }
void m_exit(id, SEL, id) {
    if (!g_win) return;
    Event e;
    e.type = Event::MouseMove;
    e.x = e.y = -1000;
    g_win->push(e);
}
void scroll(id, SEL, id ev) {
    if (!g_win) return;
    CGPoint p = reinterpret_cast<CGPoint (*)(id, SEL)>(objc_msgSend)(ev, S("locationInWindow"));
    CGRect b = msg_rect(g_win->view_, "bounds");
    Event e;
    e.type = Event::Scroll;
    e.x = float(p.x);
    e.y = float(b.size.height - p.y);
    double dx = msg<double>(ev, "scrollingDeltaX"), dy = msg<double>(ev, "scrollingDeltaY");
    if (!msg<signed char>(ev, "hasPreciseScrollingDeltas")) { dx *= 24; dy *= 24; }
    e.dx = float(dx);
    e.dy = float(dy);
    e.mods = mods_of(ev);
    g_win->push(e);
}
id make_backing_layer(id, SEL) {
    id layer = msgc(C("CAMetalLayer"), "layer");
    return layer;
}
unsigned long dragging_entered(id, SEL, id) { return 1; /*NSDragOperationCopy*/ }
signed char perform_drag(id, SEL, id info) {
    if (!g_win) return 0;
    id pb = msg(info, "draggingPasteboard");
    id cls = reinterpret_cast<id>(C("NSURL"));
    id classes = msgc(C("NSArray"), "arrayWithObject:", cls);
    id urls = msg(pb, "readObjectsForClasses:options:", classes, (id) nullptr);
    unsigned long n = urls ? msg<unsigned long>(urls, "count") : 0;
    for (unsigned long i = 0; i < n; i++) {
        id url = msg(urls, "objectAtIndex:", i);
        Event e;
        e.type = Event::Drop;
        e.text = from_ns(msg(url, "path"));
        g_win->push(e);
    }
    return 1;
}

// window delegate
signed char should_close(id, SEL, id) {
    if (g_win) { Event e; e.type = Event::Close; g_win->push(e); }
    return 0;
}
void did_resize(id, SEL, id) {
    if (!g_win) return;
    g_win->update_size();
    Event e;
    e.type = Event::Resize;
    g_win->push(e);
}
void did_become_key(id, SEL, id) { if (g_win) { Event e; e.type = Event::FocusGained; g_win->push(e); } }
void did_resign_key(id, SEL, id) { if (g_win) { Event e; e.type = Event::FocusLost; g_win->push(e); } }
signed char app_should_terminate_after_last(id, SEL, id) { return 1; }
unsigned long app_should_terminate(id, SEL, id) {
    if (g_win) { Event e; e.type = Event::Close; g_win->push(e); }
    return 0;  // NSTerminateCancel: the app's loop exits itself
}

Class view_class() {
    static Class cls = [] {
        Class c = objc_allocateClassPair(C("NSView"), "TributaryView", 0);
        class_addMethod(c, S("acceptsFirstResponder"), (IMP)yes_imp, "c@:");
        class_addMethod(c, S("acceptsFirstMouse:"), (IMP)yes1_imp, "c@:@");
        class_addMethod(c, S("isFlipped"), (IMP)yes_imp, "c@:");
        class_addMethod(c, S("wantsUpdateLayer"), (IMP)yes_imp, "c@:");
        class_addMethod(c, S("makeBackingLayer"), (IMP)make_backing_layer, "@@:");
        class_addMethod(c, S("keyDown:"), (IMP)key_down, "v@:@");
        class_addMethod(c, S("keyUp:"), (IMP)key_up, "v@:@");
        class_addMethod(c, S("performKeyEquivalent:"), (IMP)perform_key_equivalent, "c@:@");
        class_addMethod(c, S("flagsChanged:"), (IMP)flags_changed, "v@:@");
        class_addMethod(c, S("mouseDown:"), (IMP)m_down, "v@:@");
        class_addMethod(c, S("mouseUp:"), (IMP)m_up, "v@:@");
        class_addMethod(c, S("rightMouseDown:"), (IMP)rm_down, "v@:@");
        class_addMethod(c, S("rightMouseUp:"), (IMP)rm_up, "v@:@");
        class_addMethod(c, S("otherMouseDown:"), (IMP)om_down, "v@:@");
        class_addMethod(c, S("otherMouseUp:"), (IMP)om_up, "v@:@");
        class_addMethod(c, S("mouseMoved:"), (IMP)m_move, "v@:@");
        class_addMethod(c, S("mouseDragged:"), (IMP)m_move, "v@:@");
        class_addMethod(c, S("rightMouseDragged:"), (IMP)m_move, "v@:@");
        class_addMethod(c, S("otherMouseDragged:"), (IMP)m_move, "v@:@");
        class_addMethod(c, S("mouseExited:"), (IMP)m_exit, "v@:@");
        class_addMethod(c, S("scrollWheel:"), (IMP)scroll, "v@:@");
        class_addMethod(c, S("draggingEntered:"), (IMP)dragging_entered, "Q@:@");
        class_addMethod(c, S("performDragOperation:"), (IMP)perform_drag, "c@:@");
        objc_registerClassPair(c);
        return c;
    }();
    return cls;
}

Class delegate_class() {
    static Class cls = [] {
        Class c = objc_allocateClassPair(C("NSObject"), "TributaryWindowDelegate", 0);
        class_addMethod(c, S("windowShouldClose:"), (IMP)should_close, "c@:@");
        class_addMethod(c, S("windowDidResize:"), (IMP)did_resize, "v@:@");
        class_addMethod(c, S("windowDidChangeBackingProperties:"), (IMP)did_resize, "v@:@");
        class_addMethod(c, S("windowDidBecomeKey:"), (IMP)did_become_key, "v@:@");
        class_addMethod(c, S("windowDidResignKey:"), (IMP)did_resign_key, "v@:@");
        class_addMethod(c, S("applicationShouldTerminateAfterLastWindowClosed:"), (IMP)app_should_terminate_after_last, "c@:@");
        class_addMethod(c, S("applicationShouldTerminate:"), (IMP)app_should_terminate, "Q@:@");
        objc_registerClassPair(c);
        return c;
    }();
    return cls;
}

id menu_item(const char* title, const char* action, const char* key) {
    id item = msg(msgc(C("NSMenuItem"), "alloc"), "initWithTitle:action:keyEquivalent:", nsstr(title), action ? S(action) : (SEL) nullptr, nsstr(key));
    return item;
}

CocoaWindow::CocoaWindow(const std::string& title, int w, int h) {
    g_win = this;
    id app = msgc(C("NSApplication"), "sharedApplication");
    msg<signed char>(app, "setActivationPolicy:", (long)0 /*Regular*/);
    id delegate = msg(msgc(delegate_class(), "alloc"), "init");
    msg<void>(app, "setDelegate:", delegate);

    // minimal main menu: Tributary > Hide, Quit
    id menubar = msg(msgc(C("NSMenu"), "alloc"), "init");
    id app_item = msg(msgc(C("NSMenuItem"), "alloc"), "init");
    msg<void>(menubar, "addItem:", app_item);
    id app_menu = msg(msgc(C("NSMenu"), "alloc"), "init");
    msg<void>(app_menu, "addItem:", menu_item("Hide Tributary", "hide:", "h"));
    msg<void>(app_menu, "addItem:", msgc(C("NSMenuItem"), "separatorItem"));
    msg<void>(app_menu, "addItem:", menu_item("Quit Tributary", "terminate:", "q"));
    msg<void>(app_item, "setSubmenu:", app_menu);
    msg<void>(app, "setMainMenu:", menubar);

    CGRect frame{{0, 0}, {double(w), double(h)}};
    unsigned long style = 1 | 2 | 4 | 8 | (1ul << 15);  // titled, closable, miniaturizable, resizable, full-size content
    window_ = msg(msgc(C("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", frame, style,
                  (unsigned long)2 /*buffered*/, (signed char)0);
    msg<void>(window_, "setReleasedWhenClosed:", (signed char)0);
    msg<void>(window_, "setTitle:", nsstr(title));
    // GitHub Desktop draws its own dark title bar strip behind the traffic lights
    msg<void>(window_, "setTitlebarAppearsTransparent:", (signed char)1);
    msg<void>(window_, "setTitleVisibility:", (long)1 /*hidden*/);
    msg<void>(window_, "setDelegate:", delegate);
    msg<void>(window_, "setAcceptsMouseMovedEvents:", (signed char)1);
    msg<void>(window_, "setContentMinSize:", CGSize{760, 480});
    msg<void>(window_, "setFrameAutosaveName:", nsstr("TributaryMainWindow"));
    msg<void>(window_, "center");

    view_ = msg(msgc(view_class(), "alloc"), "initWithFrame:", frame);
    msg<void>(view_, "setWantsLayer:", (signed char)1);
    layer_ = msg(view_, "layer");
    msg<void>(window_, "setContentView:", view_);
    msg<signed char>(window_, "makeFirstResponder:", view_);
    // drag & drop of folders
    id types = msgc(C("NSArray"), "arrayWithObject:", nsstr("public.file-url"));
    msg<void>(view_, "registerForDraggedTypes:", types);
    // tracking area for mouseExited
    id ta = msg(msgc(C("NSTrackingArea"), "alloc"), "initWithRect:options:owner:userInfo:", CGRect{{0, 0}, {0, 0}},
                (unsigned long)(0x01 | 0x02 | 0x80 | 0x200) /*enteredExited|mouseMoved|activeAlways|inVisibleRect*/, view_,
                (id) nullptr);
    msg<void>(view_, "addTrackingArea:", ta);

    msg<void>(window_, "makeKeyAndOrderFront:", (id) nullptr);
    msg<void>(app, "finishLaunching");
    msg<void>(app, "activateIgnoringOtherApps:", (signed char)1);
    update_size();
}

}  // namespace

std::unique_ptr<Window> Window::create(const std::string& title, int w, int h) {
    return std::make_unique<CocoaWindow>(title, w, h);
}


void CocoaWindow::set_menu(const std::vector<MenuSpec>& menus) {
    static id target = msg(msgc(menu_target_class(), "alloc"), "init");
    id app = msgc(C("NSApplication"), "sharedApplication");
    id menubar = msg(msgc(C("NSMenu"), "alloc"), "init");
    for (auto& m : menus) {
        id top = msg(msgc(C("NSMenuItem"), "alloc"), "init");
        msg<void>(menubar, "addItem:", top);
        id sub = msg(msgc(C("NSMenu"), "alloc"), "initWithTitle:", nsstr(m.title));
        msg<void>(sub, "setAutoenablesItems:", (signed char)0);
        for (auto& it : m.items) {
            if (it.separator) { msg<void>(sub, "addItem:", msgc(C("NSMenuItem"), "separatorItem")); continue; }
            id item;
            if (it.command < 0) {
                // standard Cocoa actions (hide:, terminate:, …) encoded as negative ids
                static const char* std_actions[] = {"", "hide:", "hideOtherApplications:", "unhideAllApplications:", "terminate:",
                                                    "undo:", "redo:", "cut:", "copy:", "paste:", "selectAll:", "toggleFullScreen:",
                                                    "performMiniaturize:", "performZoom:"};
                int k = -it.command;
                item = menu_item(it.label.c_str(), k < int(sizeof std_actions / sizeof *std_actions) ? std_actions[k] : nullptr, it.key.c_str());
            } else {
                item = menu_item(it.label.c_str(), "menuAction:", it.key.c_str());
                msg<void>(item, "setTarget:", target);
                msg<void>(item, "setTag:", (long)it.command);
            }
            unsigned long mask = 0;
            if (it.mods & ModCmd) mask |= 1ul << 20;
            if (it.mods & ModShift) mask |= 1ul << 17;
            if (it.mods & ModAlt) mask |= 1ul << 19;
            if (it.mods & ModCtrl) mask |= 1ul << 18;
            if (!it.key.empty()) msg<void>(item, "setKeyEquivalentModifierMask:", mask);
            msg<void>(item, "setEnabled:", (signed char)(it.enabled ? 1 : 0));
            if (it.checked) msg<void>(item, "setState:", (long)1);
            msg<void>(sub, "addItem:", item);
        }
        msg<void>(top, "setSubmenu:", sub);
    }
    msg<void>(app, "setMainMenu:", menubar);
}

}  // namespace plat
#endif
