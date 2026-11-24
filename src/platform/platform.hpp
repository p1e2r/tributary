// SPDX-License-Identifier: MIT
// OS abstraction: one window, input events, clipboard, dialogs, shell helpers.
// Implementations: platform_x11.cpp, platform_win32.cpp, platform_cocoa.mm (pure
// C++ over the Objective-C runtime).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace plat {

enum class Key : uint16_t {
    Unknown, A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Enter, Escape, Backspace, Tab, Space, Delete, Insert, Home, End, PageUp, PageDown,
    Left, Right, Up, Down, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Comma, Period, Slash, Minus, Equals, Grave, LeftBracket, RightBracket, Backslash, Semicolon, Quote, Count
};

enum Mods : uint8_t { ModShift = 1, ModCtrl = 2, ModAlt = 4, ModSuper = 8 };

// "Command" modifier: Cmd on macOS, Ctrl elsewhere.
#ifdef __APPLE__
constexpr uint8_t ModCmd = ModSuper;
#else
constexpr uint8_t ModCmd = ModCtrl;
#endif

enum class Cursor { Arrow, IBeam, Hand, ResizeH, ResizeV, Wait };

struct Event {
    enum Type { Nothing, MouseMove, MouseDown, MouseUp, Scroll, KeyDown, KeyUp, Text, Resize, Close, FocusGained, FocusLost, Wake, Drop, Command } type = Nothing;
    float x = 0, y = 0;        // logical coordinates
    float dx = 0, dy = 0;      // scroll deltas (lines * 40 or precise pixels)
    int button = 0;            // 0 left, 1 right, 2 middle
    int clicks = 1;
    Key key = Key::Unknown;
    uint8_t mods = 0;
    bool repeat = false;
    std::string text;          // UTF-8 for Text events; path for Drop
    int command = 0;           // Command events: id of the chosen native menu item
};

// Native application menu (macOS menu bar). Other platforms draw their menu in-app.
struct MenuItemSpec {
    std::string label;
    int command = 0;
    std::string key;     // key equivalent shown in the menu, e.g. "n", "=" (lowercase)
    uint8_t mods = 0;    // ModCmd/ModShift/ModAlt
    bool separator = false;
    bool enabled = true;
    bool checked = false;
};
struct MenuSpec {
    std::string title;
    std::vector<MenuItemSpec> items;
};

class Window {
public:
    static std::unique_ptr<Window> create(const std::string& title, int w, int h);
    virtual ~Window() = default;

    // Blocks until at least one event arrives (or timeout_ms passes; -1 = forever).
    virtual void wait_events(int timeout_ms) = 0;
    virtual bool poll(Event& e) = 0;
    // Thread-safe: wakes wait_events() from another thread.
    virtual void wake() = 0;

    virtual int pixel_width() const = 0;
    virtual int pixel_height() const = 0;
    virtual float scale() const = 0;  // pixels per logical unit
    virtual void set_title(const std::string& t) = 0;
    virtual void set_cursor(Cursor c) = 0;
    virtual void set_text_input_rect(float x, float y, float w, float h) { (void)x; (void)y; (void)w; (void)h; }

    virtual std::string clipboard_get() = 0;
    virtual void clipboard_set(const std::string& s) = 0;

    // Vulkan glue
    virtual std::vector<const char*> vk_instance_extensions() const = 0;
    virtual VkSurfaceKHR vk_create_surface(VkInstance inst, PFN_vkGetInstanceProcAddr gipa) = 0;

    // Native dialogs; nullopt if cancelled or unsupported (the app falls back
    // to its own in-app path field).
    virtual std::optional<std::string> pick_folder(const std::string& title, const std::string& start) = 0;
    virtual bool is_dark_mode() const { return true; }
    // Replace the native menu bar; a no-op where menus are drawn by the app.
    virtual void set_menu(const std::vector<MenuSpec>& menus) { (void)menus; }
    virtual bool has_native_menu() const { return false; }
    // Height of the window's own title bar area that the app draws itself
    // (macOS: transparent full-size title bar with the traffic lights in it).
    virtual float titlebar_height() const { return 0; }
};

// ---------------------------------------------------------------- pseudo-terminals
// A child process (usually a shell) attached to a pseudo-terminal: POSIX ptys on
// macOS/Linux, ConPTY on Windows 10 1809+. A reader thread collects output.
class Pty {
public:
    static std::unique_ptr<Pty> spawn(const std::vector<std::string>& argv, const std::string& cwd, int cols, int rows,
                                      std::string* error = nullptr);
    virtual ~Pty() = default;
    virtual void write(std::string_view data) = 0;
    virtual void resize(int cols, int rows) = 0;
    virtual std::string take_output() = 0;  // drains output collected since the last call
    virtual bool alive() const = 0;
    virtual int exit_code() const = 0;
    virtual void kill() = 0;
    std::function<void()> on_data;  // called on the reader thread; use it to wake the UI
};
std::vector<std::string> default_shell();

// Shell integration
void open_url(const std::string& url);
void reveal_in_file_manager(const std::string& path);
void open_in_terminal(const std::string& path, const std::string& custom_cmd = "");
void open_in_editor(const std::string& path, const std::string& custom_cmd = "");
std::string app_data_dir();       // per-user, created on demand
std::string default_clone_dir();  // ~/Documents/GitHub like GitHub Desktop
std::string os_name();
std::string arch_name();        // "x86_64", "arm64"
std::string executable_path();  // absolute path of the running binary
// Run a command with the system shell, detached.
void spawn_detached(const std::vector<std::string>& argv);

// Move a file or folder to the OS trash / recycle bin. Returns false on failure.
bool move_to_trash(const std::string& path);

// Secure storage for tokens (Keychain / Windows Credential Manager / 0600 file).
std::string secret_get(const std::string& key);
void secret_set(const std::string& key, const std::string& value);
void secret_delete(const std::string& key);

// Monotonic time in seconds.
double now_seconds();

// Vulkan library (loaded dynamically; no link-time dependency).
PFN_vkGetInstanceProcAddr load_vulkan();

}  // namespace plat
