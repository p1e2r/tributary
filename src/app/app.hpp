// SPDX-License-Identifier: MIT
// Tributary application state and top-level frame.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "../git/transport.hpp"
#include "../net/github.hpp"
#include "../gfx/image.hpp"
#include "../ui/ui.hpp"
#include "../ui/code_editor.hpp"
#include "../ui/terminal.hpp"

namespace app {

using git::ObjectId;
using ui::Rect;

// ---------------------------------------------------------------- settings
struct Settings {
    std::string theme = "system";  // light | dark | system
    std::string editor_cmd, shell_cmd;
    std::string clone_dir;
    std::string oauth_client_id;
    std::string default_branch = "main";
    bool confirm_discard = true, confirm_force_push = true, confirm_remove_repo = true;
    float sidebar_width = 300;
    bool split_diff = false, hide_whitespace = false;
    float zoom = 1.0f;
    int image_mode = 0;  // 0 2-up, 1 swipe, 2 onion skin, 3 difference
};

struct RepoEntry {
    std::string path, name;
    std::string owner;  // GitHub owner if any (for grouping)
    bool missing = false;
    bool pinned = false;
    // background indicators shown in the repository list
    int changes = 0, ahead = 0, behind = 0;
    bool indicators = false;
};

// ---------------------------------------------------------------- per-repo view state
struct FileSel {
    bool included = true;
    std::optional<std::vector<bool>> lines;  // partial selection over full diff
};

struct DiffView {
    std::string path;               // which file this diff is for
    git::FileDiff diff;
    std::vector<git::DiffLine> full;  // full diff lines (selection index space)
    bool loading = false;
    std::string error;
    bool image = false;
    uint64_t token = 0;
    std::shared_ptr<gfx::Image> img_old, img_new, img_diff;
    std::string img_error;
    size_t old_bytes = 0, new_bytes = 0;
    bool ignore_ws = false;
};

struct RepoState {
    std::string path, name;
    bool loaded = false, loading = false;
    std::string error;
    // snapshot from the worker
    git::WorkingStatus status;
    std::vector<git::BranchInfo> branches;
    std::string branch;        // current branch short name ("" = detached)
    std::optional<ObjectId> head;
    std::string head_summary;
    std::vector<git::Remote> remotes;
    std::vector<git::StashInfo> stashes;
    std::string upstream;      // refs/remotes/origin/main
    int ahead = 0, behind = 0;
    std::string default_branch;  // from origin/HEAD
    std::optional<std::pair<std::string, std::string>> github;  // owner, name
    int64_t last_fetched = 0;
    std::vector<std::pair<std::string, ObjectId>> tags;

    // changes tab
    std::map<std::string, FileSel> sel;
    std::string selected_file;
    std::set<std::string> multi_selected;
    DiffView diff;
    std::string summary, description;
    bool amending = false;
    std::optional<ObjectId> last_commit_for_undo;
    std::string last_commit_summary;

    // history tab
    std::vector<git::CommitInfo> history;
    bool history_complete = false, history_loading = false;
    std::optional<ObjectId> selected_commit;
    std::vector<git::ChangedFile> commit_files;
    std::string selected_commit_file;
    DiffView commit_diff;
    std::string history_filter;

    // stash view
    bool viewing_stash = false;
    std::vector<git::ChangedFile> stash_files;
    std::string selected_stash_file;
    DiffView stash_diff;

    // pull requests
    std::vector<gh::PullRequest> prs;
    bool prs_loading = false;
    int64_t prs_loaded_at = 0;
    // CI checks for HEAD
    gh::Checks checks;
    std::string checks_sha;
    bool checks_loading = false;
    double checks_at = 0;
    int last_check_state = -1;
    // fork handling
    bool fork_contrib = false;   // PRs / default branch come from the parent ("upstream" remote)
    // co-authors
    bool show_coauthors = false;
    std::string coauthors;       // "@login, Name <email>"
    // history selection
    std::set<ObjectId> history_sel;
    int history_anchor = -1;
    std::set<std::string> files_sel;  // multi-select in the changes list
    int files_anchor = -1;
};

// ---------------------------------------------------------------- jobs
// All git work runs on a single worker thread; results are applied on the UI
// thread through post().
class Jobs {
public:
    explicit Jobs(plat::Window& w);
    ~Jobs();
    void run(std::string label, std::function<void()> fn);  // runs on worker
    void post(std::function<void()> fn);                      // runs on UI thread
    void drain();                                              // UI thread: apply posted results
    bool busy() const { return busy_.load(); }
    std::string current_label();
    void set_progress(const std::string& text, float fraction);
    std::pair<std::string, float> progress();

private:
    plat::Window& win_;
    std::thread th_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, std::function<void()>>> q_;
    std::vector<std::function<void()>> results_;
    std::atomic<bool> busy_{false}, quit_{false};
    std::string label_, progress_text_;
    float progress_ = -1;
};

// ---------------------------------------------------------------- dialogs
enum class Dialog {
    None, Clone, Create, AddExisting, NewBranch, RenameBranch, DeleteBranch, Merge, Publish, Settings,
    RepoSettings, About, Error, ConfirmDiscard, Conflicts, SignIn, Credentials, ConfirmRemoveRepo,
    CreateTag, ForcePush, Welcome, SwitchBranch, Reword, Squash, CherryPick, PrPreview, ResetToCommit,
    LargeFiles, ForkChoice, ConfirmRewrite, DeleteTag, Checks,
    IdeOpen, IdeSubmit, IdeNewFile, IdeConfirmDiscard
};

struct AppMenuItem {
    std::string label;
    plat::Key key = plat::Key::Unknown;  // accelerator
    uint8_t mods = 0;
    std::function<void()> fn;
    bool enabled = true;
    bool sep = false;
    int native = 0;  // negative: standard Cocoa action (see platform_cocoa set_menu)
};
struct AppMenu {
    std::string title;
    std::vector<AppMenuItem> items;
};

// ---------------------------------------------------------------- editing on GitHub
struct IdeFile {
    std::string path, base_sha, mode = "100644";
    int64_t size = 0;
    enum State { NotLoaded, Loading, Loaded, Failed } state = NotLoaded;
    bool binary = false, added = false, deleted = false;
    std::string original;           // text when loaded (empty for new files)
    std::string raw;                // bytes of binary files
    std::string copy_from_sha;      // added by a rename: content comes from this blob
    std::string renamed_from;       // ... and this path was deleted by the rename
    std::shared_ptr<gfx::Image> image;
    ui::CodeDoc doc;
    std::string error;
    uint64_t checked_version = ~0ull;
    bool modified = false;
};
struct IdeState {
    bool open = false;
    std::string owner, repo, base, default_branch;
    bool loading = false, can_push = true;
    std::string error;
    std::string commit_sha;
    bool truncated = false;
    std::vector<std::string> branches;
    std::map<std::string, IdeFile> files;
    std::vector<std::string> tabs;
    std::string active;
    std::set<std::string> expanded;
    int side_tab = 0;
    std::string filter, tree_sel;
    std::string change_sel;
    DiffView change_diff;
    uint64_t change_diff_ver = ~0ull;
    uint64_t generation = 0;
    uint64_t tree_ver = 1, tree_built = 0;
    struct Node { std::string path, name; int depth; bool dir; };
    std::vector<Node> nodes;
};

struct Toast {
    std::string text;
    bool error = false;
    double until = 0;
    std::string action_label;
    std::function<void()> action;
};

class App {
public:
    App(plat::Window& w, gfx::Renderer& r);
    ~App();
    void frame();
    void on_event(const plat::Event& e);
    bool should_quit = false;
    ui::Context ui;

    // ---- shared state (UI thread)
    Settings settings;
    std::vector<RepoEntry> repos;
    int current = -1;
    std::unique_ptr<RepoState> rs;  // state of the current repo
    gh::Account account;      // github.com
    gh::Account enterprise;   // GitHub Enterprise (optional)
    const gh::Account* account_for_url(const std::string& url) const;
    std::vector<gh::RepoSummary> gh_repos;
    bool gh_repos_loading = false;
    int64_t gh_repos_at = 0;
    int tab = 0;  // 0 changes, 1 history
    Jobs jobs;
    std::vector<Toast> toasts;

    // dialogs
    Dialog dialog = Dialog::None;
    std::string error_title, error_text;
    struct {
        int tab = 0;
        std::string url, path, filter;
        int selected = -1;
        bool path_edited = false;
    } clone_d;
    struct {
        std::string name, description, path;
        bool readme = true, mit = true, gitignore = true;
    } create_d;
    struct { std::string path; } add_d;
    struct {
        std::string name;
        int from = 0;  // 0 current branch, 1 default branch
        std::optional<ObjectId> from_commit;
    } branch_d;
    struct { std::string from, to; } rename_d;
    struct { std::string name; bool remote = false; } delete_d;
    struct {
        std::string filter, selected;
        int mode = 0;  // 0 merge, 1 squash and merge, 2 rebase
        std::string previewed;
        git::MergePreview preview;
        bool preview_loading = false;
        int rebase_commits = 0;
    } merge_d;
    struct { std::string name, description, org; bool priv = true; std::vector<std::string> orgs; } publish_d;
    struct { int tab = 0; std::string token, name, email, default_branch, signing_key; bool loaded = false, sign = false; int sign_format = 0; } settings_d;
    struct { int tab = 0; std::string remote_url, gitignore; bool loaded = false; } repo_settings_d;
    struct { std::vector<std::string> paths; } discard_d;
    struct { std::string name; ObjectId target; } tag_d;
    struct {
        std::string url, user, pass;
        std::promise<std::optional<git::Credentials>>* promise = nullptr;
    } cred_d;
    struct {
        gh::DeviceCode code;
        bool waiting = false;
        std::string status;
    } signin_d;
    struct { int remove_index = -1; } remove_d;
    struct { std::string target; int choice = 0; } switch_d;
    struct { ObjectId commit; std::string summary, description; } reword_d;
    struct { std::vector<ObjectId> commits; ObjectId into; std::string summary, description; } squash_d;
    struct { std::vector<ObjectId> commits; std::string filter, selected; } cherry_d;
    struct {
        std::string base, filter;
        bool loading = false;
        int commits = 0;
        std::vector<git::ChangedFile> files;
        std::string selected;
        DiffView diff;
        std::string error;
    } pr_d;
    struct { ObjectId commit; } reset_d;
    struct { std::vector<std::pair<std::string, uint64_t>> files; } large_d;
    struct { std::string parent, parent_url, parent_default; std::string path; int choice = 0; } fork_d;
    struct { std::function<void()> go; std::string what; } rewrite_d;
    struct { std::string name; bool remote = false; } deltag_d;
    struct { int signin_tab = 0; std::string ghe_url; } ghe_d;

    // context menu
    struct MenuEntry {
        std::string label;
        std::function<void()> fn;
        bool enabled = true, separator = false;
        std::string shortcut;
    };
    std::vector<MenuEntry> ctx_items;
    float ctx_x = 0, ctx_y = 0;
    void open_context(std::vector<MenuEntry> items);
    void draw_context_menu();
    float diff_hscroll = 0;
    int drag_select_value = -1;  // diff gutter drag-select (0/1), -1 idle
    std::set<int> drag_seen;
    // diff search & text selection
    bool find_open = false;
    std::string find_text;
    int find_current = 0;
    int text_sel_a = -1, text_sel_b = -1;  // selected row range in the diff
    bool text_selecting = false;
    std::map<std::string, int> diff_context;  // per-file expanded context
    float image_split = 0.5f, image_opacity = 0.5f;
    // history drag & drop
    struct {
        bool active = false, pending = false;
        float start_x = 0, start_y = 0;
        std::vector<ObjectId> commits;
        int drop_index = -1;        // insert before this row (reorder)
        int squash_index = -1;      // drop onto this row (squash)
        std::string branch_target;  // drop onto a branch (cherry-pick)
    } hdrag;

    // popups
    std::string repo_filter, branch_filter;
    int branch_tab = 0;

    // ---- actions
    void open_repo(int index);
    void add_repo_path(const std::string& path, bool select = true);
    void remove_repo(int index);
    void toggle_pin(int index);
    void refresh(bool full = true);
    void load_history(bool more);
    void select_file(const std::string& path);
    void select_commit(const ObjectId& id);
    void select_commit_file(const std::string& path);
    void load_stash_view();
    void commit();
    void undo_commit();
    void fetch();
    void pull();
    void push(bool force = false);
    void publish_branch();
    void checkout(const std::string& branch);
    void create_branch(const std::string& name, std::optional<ObjectId> from);
    void delete_branch(const std::string& name, bool remote);
    void rename_branch(const std::string& from, const std::string& to);
    void merge_into_current(const std::string& ref);
    void discard(const std::vector<std::string>& paths);
    void discard_lines(const std::string& path, const std::vector<bool>& selection);
    void stash_all();
    void stash_restore(bool pop);
    void stash_drop();
    void revert_commit(const ObjectId& id);
    void checkout_pr(const gh::PullRequest& pr);
    void clone_repo(const std::string& url, const std::string& dir);
    void create_repo();
    void publish_repo();
    void sign_in_with_token(const std::string& token);
    void start_device_flow();
    void sign_out();
    void load_gh_repos(bool force = false);
    void load_prs(bool force = false);
    void abort_merge();
    void rebase_onto(const std::string& ref);
    void squash_merge(const std::string& ref);
    void run_rewrite(const std::string& label, std::function<git::RewriteResult(git::Repository&)> fn);
    void rewrite_continue();
    void rewrite_abort();
    void reword_commit(const ObjectId& id, const std::string& message);
    void squash_commits(const std::vector<ObjectId>& commits, const ObjectId& into, const std::string& message);
    void reorder_commits(const std::vector<ObjectId>& commits, const ObjectId& before);
    void cherry_pick(const std::vector<ObjectId>& commits, const std::string& branch);
    void reset_to_commit(const ObjectId& id);
    void delete_tag(const std::string& name, bool remote);
    void load_checks(bool force = false);
    void rerun_checks();
    void load_repo_indicators();
    void load_pr_preview();
    void set_zoom(float z);
    void do_checkout(const std::string& branch, bool stash_first);
    void confirm_rewrite(bool touches_pushed, const std::string& what, std::function<void()> go);
    bool commit_is_pushed(const ObjectId& id) const;
    std::vector<ObjectId> history_selection_or(const ObjectId& id) const;
    void undo_rewrite_toast(const std::string& label);
    void check_fork(const std::string& path);
    void resolve_conflict(const std::string& path, int stage);
    void show_error(const std::string& title, const std::string& text);
    void toast(const std::string& text, bool error = false);
    void save_state();
    void load_state();
    void apply_theme();
    git::CredentialProvider credentials();

    // ---- views (views.cpp / dialogs.cpp)
    void draw_toolbar(const Rect& r);
    void draw_repo_popup(const Rect& anchor);
    void draw_branch_popup(const Rect& anchor);
    void draw_sidebar(const Rect& r);
    void draw_changes_list(const Rect& r);
    void draw_commit_box(const Rect& r);
    void draw_history_list(const Rect& r);
    void draw_main(const Rect& r);
    void draw_diff(const Rect& r, DiffView& dv, bool selectable, const std::string& file_path, const std::string& header_extra);
    void draw_no_changes(const Rect& r);
    void draw_commit_details(const Rect& r);
    void draw_stash_view(const Rect& r);
    void draw_welcome(const Rect& r);
    void draw_conflict_banner(const Rect& r);
    void draw_dialogs();
    void draw_toasts();
    void draw_image_diff(const Rect& r, DiffView& dv);
    void draw_find_bar(const Rect& r);
    void draw_rewrite_banner(const Rect& r);
    void draw_multi_selection(const Rect& r);
    void draw_checks_popup(const Rect& anchor);
    void handle_shortcuts();
    std::vector<AppMenu> build_menus();
    void draw_menu_bar(const Rect& rc);
    void sync_native_menu();
    void run_menu_command(int command);
    std::vector<AppMenu> menus_;
    // VS Code-style terminal panel (Ctrl+`)
    struct TerminalPanel {
        bool open = false;
        float height = 260;
        std::vector<std::unique_ptr<ui::TerminalSession>> tabs;
        int active = 0;
    } term_panel;
    ui::Id terminal_focus_id_ = 0;
    float panel_inset_ = 0;  // height the terminal panel takes from the content area
    std::string terminal_cwd();
    void toggle_terminal();
    void new_terminal();
    Rect draw_terminal_panel(const Rect& content);  // returns what's left above the panel
    IdeState ide;
    uint64_t ide_generation_ = 0;
    struct { std::string branch, title, body; bool draft = false, submitting = false; std::string progress, error; } ide_pr_d;
    struct { std::string repo, filter; int selected = -1; } ide_open_d;
    struct { std::string path, from; bool rename = false; } ide_name_d;
    std::function<void()> ide_after_discard;
    void open_ide(const std::string& owner, const std::string& repo, const std::string& branch = "");
    void ide_open_file(const std::string& path, bool focus = true);
    void ide_load(const std::string& path);
    void ide_close_tab(const std::string& path);
    std::vector<std::pair<std::string, git::Change>> ide_changes();
    void ide_discard(const std::string& path);
    void ide_delete(const std::string& path);
    void ide_rename(const std::string& from, const std::string& to);
    void ide_new_file(const std::string& path);
    void ide_submit();
    void ide_guard(std::function<void()> fn);  // runs fn, asking first if there are unsubmitted edits
    void draw_ide_toolbar(const Rect& rc);
    void draw_ide(const Rect& rc);
    void draw_ide_tree(const Rect& rc);
    void draw_ide_changes(const Rect& rc);
    void draw_ide_editor(const Rect& rc);
    void draw_ide_dialogs();
    // GitHub avatars by email (avatars.githubusercontent.com/u/e?email=…), like Desktop
    struct AvatarEntry { int state = 0; std::shared_ptr<gfx::Image> img; };  // 0 queued, 1 loading, 2 done
    std::map<std::string, AvatarEntry> avatars_;
    std::vector<std::string> avatar_queue_;
    bool avatar_worker_ = false;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    void draw_avatar(const Rect& rc, const std::string& name, const std::string& email);
    void pump_avatars();
    std::map<std::string, std::shared_ptr<gfx::Image>> illustrations_;
    void illustration(const std::string& name, const Rect& rc, float alpha = 1.f, bool dark_filter = true);
    ui::Id summary_id = 0;
    uint64_t native_menu_sig_ = 0;
    int menubar_open_ = -1;
    std::vector<ui::KeyPress> injected_keys;
    bool list_focus_changes = false, list_focus_history = false;

    std::string repo_display_path(const std::string& p) const;
    bool has_changes() const { return rs && !rs->status.files.empty(); }
    std::string remote_name() const;
    std::string github_url() const;

    plat::Window& win;
    gfx::Renderer& r;
    double last_status_refresh = 0;
    double last_auto_fetch = 0;
    uint64_t diff_token = 0;
    std::string state_file;
    std::string git_user_name, git_user_email;
    void load_git_identity();
};

}  // namespace app
