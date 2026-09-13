/*
    Simple Framework Cpp

    Tianming Wu <github.com/Tianming-Wu> 2026.08.21

*/

#pragma once

#include <memory>
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <functional>
#include <future>
#include <utility>
#include <vector>

#include <SharedCppLib2/typemask.hpp>
#include <SharedCppLib2/scltypes.hpp>
#include <SharedCppLib2/bitmap.hpp>
#include <SharedCppLib2/color.hpp>
#include <SharedCppLib2/string.hpp>

// Windows DLL export/import. SCF_CPP is defined when building the scf DLL (by
// CMake) so classes get dllexport there and dllimport for consumers. For a
// static build define SCF_STATIC (see the scf_static target): classes are
// then compiled/used without any decoration.
#if defined(SCF_STATIC)
#  define SCF_EXPORT
#elif defined(_WIN32)
#  if defined(SCF_CPP)
#    define SCF_EXPORT __declspec(dllexport)
#  else
#    define SCF_EXPORT __declspec(dllimport)
#  endif
#else
#  define SCF_EXPORT
#endif

namespace scf {

class window;
class control;

typedef scl2::dword_t control_id_t;
typedef uint16_t event_id_t;

enum class WindowFlags : uint32_t {
    None = 0,
    Frameless = 1 << 0,      // Frameless window (WS_POPUP)
    rightClickExit = 1 << 1, // Right-click closes the window (WM_CLOSE)
    FixedSize = 1 << 2,      // Not resizable by the user: no WS_THICKFRAME and
                             // no maximize box. resize() still works.
};

// Bitwise combinators so flags read naturally: Frameless | rightClickExit.
inline constexpr WindowFlags operator|(WindowFlags a, WindowFlags b) noexcept
{
    return static_cast<WindowFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline constexpr WindowFlags operator&(WindowFlags a, WindowFlags b) noexcept
{
    return static_cast<WindowFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

/*
    Control is sublevel of a window, it can be a button, a label, a text box, etc.
    We use Windows' Common Control library to implement these controls. Their
    actions get directly sent to the main window, so this class is more of a configuration
    tool for them.
*/
class SCF_EXPORT control {
    friend class window;
    friend class radiogroup;   // container controls lay out sibling sub-controls
public:
    // Ask the owning window to close itself. Handy for a button that finishes
    // a dialog (e.g. OK/Cancel). Safe to call from any thread.
    void close_owner();

protected:
    // `extra_style` is the class-specific creation style, applied once at
    // CreateWindow time (e.g. BS_AUTOCHECKBOX for a checkbox). It lives in
    // the ordinary (non-extended) style word; `extra_ex_style` is the
    // extended style (WS_EX_*, e.g. WS_EX_CLIENTEDGE for a bordered textbox).
    control(scl2::wstring class_name, scl2::dword_t extra_style, scl2::Geometry geometry);
    control(scl2::wstring class_name, scl2::dword_t extra_style, scl2::Geometry geometry, scl2::wstring text);
    control(scl2::wstring class_name, scl2::dword_t extra_style, scl2::dword_t extra_ex_style, scl2::Geometry geometry);
    control(scl2::wstring class_name, scl2::dword_t extra_style, scl2::dword_t extra_ex_style, scl2::Geometry geometry, scl2::wstring text);
    virtual ~control() = default;

    // Text read/write, redirected to the Win32 API. Writing temporarily
    // suppresses the resulting change notifications so programmatic updates
    // do not fire the user event callbacks (the re-entrancy problem).
    // Protected: subclasses decide whether to expose them (e.g. a numeric-only
    // edit may not want a generic text API), via `using control::set_text;` or
    // a public wrapper.
    void set_text(const scl2::wstring& text);
    scl2::wstring get_text() const;

    void create_control(control_id_t cid, scl2::winhandle_t parent_hwnd);

    // Modify the control's Win32 style bits; only bits set in `mask` are
    // changed (default: all). Use a precomputed mask (e.g. SS_TYPEMASK) to
    // touch just one style group (like alignment) without clobbering the rest
    // -- including the class-specific creation style (extra_style).
    void set_style(scl2::dword_t style, scl2::dword_t mask = ~0u);

    // Called right after the control's HWND is created (during
    // generate_children / WM_CREATE). Subclasses override it to apply
    // pre-creation state that cannot be expressed as a style bit, e.g. a
    // checkbox's checked state captured before show().
    virtual void on_created() {}

    // The OS window handle; null until the control is created (WM_CREATE).
    // Subclasses use it to send control-specific messages (BM_SETCHECK,
    // EM_SETREADONLY, ...) to the real window.
    scl2::winhandle_t m_hwnd {nullptr};

    // The control's geometry, relative to the window's client area (logical
    // 100% scale). Protected so subclasses can lay out sub-controls (e.g. a
    // radiogroup positions its radios inside its own rect).
    scl2::Geometry m_geometry;

    // Sub-controls that must be created as sibling windows of this control
    // (e.g. a radiogroup's radios). The owning window registers them like any
    // other child; empty by default.
    virtual void collect_sub_controls(std::vector<std::shared_ptr<control>>& out) const {}

    void set_event(event_id_t event_id, std::function<void()> fx);
    void unset_event(event_id_t event_id);

    // Runs a programmatic control write (SendMessage / SetWindowText / ...)
    // with change-notification suppression, so the triggered notifications are
    // not delivered as user events. Exception-safe.
    template <typename Fn>
    void sys_change(Fn&& fn) {
        m_suppress_events.store(true);
        try {
            fn();
        } catch (...) {
            m_suppress_events.store(false);
            throw;
        }
        m_suppress_events.store(false);
    }

private:
    // The window that owns this control (set by window::add_child, refreshed
    // on move). Raw pointer: a control lives inside its window's impl, so the
    // window always outlives it -- no ownership cycle.
    window* m_owner = nullptr;
    control_id_t control_id;

    scl2::wstring m_class_name;
    scl2::dword_t m_extra_style;    // Class-specific style, applied once at CreateWindow.
    scl2::dword_t m_extra_ex_style; // Extended style (WS_EX_*), applied once at CreateWindow.
    scl2::wstring m_text;

    // Style changes requested before the HWND exists (set_style/set_alignment
    // before show()); composed and applied in create_control().
    scl2::dword_t m_pending_style{0};
    scl2::dword_t m_pending_mask{0};

    std::map<event_id_t, std::function<void()>> _event_callbacks;


    // Called by the owning window to dispatch a WM_COMMAND notification code
    // (e.g. BN_CLICKED) to the matching event callback.
    void handle_event(event_id_t notification);

    // Set while the program itself changes the control (creation, set_text);
    // suppresses the resulting change notifications in handle_event().
    std::atomic<bool> m_suppress_events{false};
};

class SCF_EXPORT button : public control {
public:
    button(scl2::Geometry geometry, scl2::wstring text = L"");
    virtual ~button() = default;

    using control::set_text;
    using control::get_text;

    void on_click(std::function<void()> fx);
};

// Factory helper macro: generates `new_<Class>(Args&&...)` returning a
// shared_ptr<Class>, so users can hand it to window::add_child without
// spelling std::make_shared. Arguments are perfectly forwarded; constructor
// default arguments just work.
#define SCF_NEW_CONTROL(Class)                                                \
    template <typename... Args>                                               \
    inline std::shared_ptr<Class> new_##Class(Args&&... args) {               \
        return std::make_shared<Class>(std::forward<Args>(args)...);          \
    }

SCF_NEW_CONTROL(button)

class SCF_EXPORT label : public control {
public:
    label(scl2::Geometry geometry, scl2::wstring text = L"");
    virtual ~label() = default;

    using control::set_text;
    using control::get_text;

    // Text alignment inside the label (scl2::Alignment bit flags).
    // Default when never called: left + top (SS_LEFT | SS_TOP), the Win32
    // STATIC default.
    void set_alignment(scl2::Alignment a);

    // Word wrap (SS_EDITCONTROL): off by default (single line). On, lines
    // break at word boundaries / CJK characters.
    void set_wrap(bool wrap);
};

SCF_NEW_CONTROL(label)

class SCF_EXPORT checkbox : public control {
public:
    checkbox(scl2::Geometry geometry, scl2::wstring text = L"");
    virtual ~checkbox() = default;

    using control::set_text;
    using control::get_text;

    // Check-box state. Safe to call before the window is shown: the value is
    // remembered and applied when the control is created.
    bool is_checked() const;
    void set_checked(bool checked);

    void on_click(std::function<void()> fx);   // BN_CLICKED

protected:
    void on_created() override;

private:
    bool m_checked{false}; // Pre-creation state, applied in on_created().
};

SCF_NEW_CONTROL(checkbox)

class SCF_EXPORT textbox : public control {
public:
    textbox(scl2::Geometry geometry, scl2::wstring text = L"");
    virtual ~textbox() = default;

    using control::set_text;
    using control::get_text;

    // Edit notifications (all delivered on the worker thread).
    void on_change(std::function<void()> fx);     // EN_CHANGE
    void on_update(std::function<void()> fx);     // EN_UPDATE
    void on_setfocus(std::function<void()> fx);   // EN_SETFOCUS
    void on_killfocus(std::function<void()> fx);  // EN_KILLFOCUS

    // Enter key pressed in the (single-line) edit. Implemented via window
    // subclassing, installed only when this is used.
    void on_enter(std::function<void()> fx);

    // Read-only edit. Safe to call before the window is shown.
    void set_read_only(bool ro);

    // Password (bullet) display; toggles ES_PASSWORD. Safe before show().
    void set_password(bool pw);

protected:
    void on_created() override;

private:
    bool m_read_only{false};  // Pre-creation state, applied in on_created().
    bool m_want_enter{false}; // Install the Enter-key subclass at creation.
};

SCF_NEW_CONTROL(textbox)

class SCF_EXPORT groupbox : public control {
public:
    groupbox(scl2::Geometry geometry, scl2::wstring text = L"");
    virtual ~groupbox() = default;

    using control::set_text;
    using control::get_text;
};

SCF_NEW_CONTROL(groupbox)

class SCF_EXPORT radio : public control {
public:
    radio(scl2::Geometry geometry, scl2::wstring text = L"");
    virtual ~radio() = default;

    using control::set_text;
    using control::get_text;

    // Radio state. Mutual exclusion with its group is handled by Windows
    // (the group's first radio carries WS_GROUP).
    bool is_checked() const;
    void set_checked(bool checked);

    void on_click(std::function<void()> fx);   // BN_CLICKED

protected:
    void on_created() override;

private:
    bool m_checked{false}; // Pre-creation state, applied in on_created().
};

SCF_NEW_CONTROL(radio)

// A group box that lays out and manages a mutually-exclusive set of radio
// buttons inside its frame. Add the group to the window; its radios are
// registered as window children automatically (see collect_sub_controls).
class SCF_EXPORT radiogroup : public control {
public:
    radiogroup(scl2::Geometry geometry, scl2::wstring title = L"");
    virtual ~radiogroup() = default;

    // Appends a radio option (stacked vertically inside the frame) and
    // returns it. Note: setting on_click on a returned radio would replace
    // the group's internal forwarding, so use on_select() instead.
    std::shared_ptr<radio> add(const scl2::wstring& text);

    size_t size() const { return m_radios.size(); }
    std::shared_ptr<radio> at(size_t i) { return m_radios.at(i); }

    int selected_index() const;   // -1 if none selected
    void select(int index);       // -1 clears the selection

    // Fired (worker thread) when the user clicks a radio; argument = index.
    void on_select(std::function<void(int)> fx);

protected:
    void collect_sub_controls(std::vector<std::shared_ptr<control>>& out) const override;

private:
    std::vector<std::shared_ptr<radio>> m_radios;
    std::function<void(int)> m_on_select;
};

SCF_NEW_CONTROL(radiogroup)



class SCF_EXPORT window {
public:
    // Forward-declared shared state; the definition lives in scf.cpp. It is
    // public only so the window procedure can reach it; it remains an
    // incomplete type for everyone outside the implementation.
    struct impl;

    window();
    window(scl2::Geometry geometry);
    window(scl2::Geometry geometry, const std::wstring& title);
    window(scl2::Geometry geometry, const std::wstring& title, WindowFlags flags);

    ~window();

    // windows are not copyable, but they are movable: moving simply hands the
    // shared implementation (worker thread / OS window) to the new object; the
    // source is left empty and safe to destroy.
    window(const window&) = delete;
    window& operator=(const window&) = delete;
    window(window&& other) noexcept;
    window& operator=(window&& other) noexcept;

    // By default the geometry passed to the constructors is the CLIENT-area
    // size: create_window() adds the non-client area (caption + borders,
    // which vary per system) via AdjustWindowRectEx. Call with false to treat
    // the geometry as the raw outer window size instead. Must be set before
    // show().
    void use_client_geometry(bool on);

    // Restricts frameless dragging to this client-area zone: a left press
    // inside it starts a window drag (via the system's caption handler).
    // A zero/empty rect disables it. Set before show().
    void set_drag_zone(const scl2::Rect& zone);

    // Programmatically resize to a logical client size (or outer size if
    // use_client_geometry(false)). Works before or after show(); keeps the
    // top-left corner fixed. Not affected by keepAspectRatio.
    void resize(int width, int height);

    // Center the window on the primary work area. Respects the client-geometry
    // setting (centers the client area by default).
    void center_window();

    // Preserve the client aspect ratio while the user drags the window border.
    // resize() is unaffected and may set any size; the ratio only constrains
    // interactive resizing. Set before show().
    void set_keep_aspect_ratio(bool on);

    // Stops the user from resizing the window: WS_THICKFRAME is dropped (no
    // draggable borders, no resize cursor) and WS_MAXIMIZEBOX goes with it so
    // the maximize button is disabled too. Programmatic resize() keeps working.
    // Set before show(), same as the WindowFlags::FixedSize constructor flag.
    void set_fixed_size(bool on);

    // Starts the worker thread (on first call) and creates the OS window
    // WITHOUT showing it. Blocks until the window exists; safe to call
    // repeatedly. Useful for tray/background apps that need a hidden window
    // as a message pump but never want it visible. show() is start() + reveal.
    void start();

    // Starts the worker thread (on first call) and asks the window to be
    // shown. Blocks until the OS window exists; safe to call repeatedly.
    void show();
    void hide();

    // Asynchronously requests the window to close. The message loop exits once
    // the window is destroyed; the destructor joins the worker thread.
    void close();

    // Blocks the calling thread until the window is closed and the worker
    // thread has exited. Idempotent. Must not be called from a callback (that
    // runs on the worker thread itself).
    void wait_for_closed();

    // Requests a repaint (WM_PAINT) on the next idle moment.
    void request_render();

    // Draws a 1-bit bitmap stretched to the client area. Only meaningful from
    // an `on_render` callback (i.e. on the worker thread, while WM_PAINT is
    // being handled).
    void paint_bitmap(const scl2::bitmap_1c& bmp);

    // Draws a color (RGBA8) bitmap stretched to the client area (Contain,
    // letterboxed). Same thread constraints as the 1-bit overload.
    void paint_bitmap(const scl2::bitmap<scl2::rgba8>& bmp);

    // Callbacks run on the worker thread, inside the message loop. The close
    // callback receives the (current) window so dialogs can read their state
    // to produce a result.
    using render_callback = std::function<void(window&)>;
    using close_callback = std::function<void(window&)>;
    void on_render(render_callback cb);
    void on_close(close_callback cb);

    // Window-state callbacks, all invoked on the worker thread. Registered and
    // fired under the internal lock (copied out, invoked lock-free), so a
    // handler may safely call other window methods without deadlock.
    using resize_callback = std::function<void(window&, int width, int height)>; // logical client size
    using move_callback = std::function<void(window&, int x, int y)>;            // screen position
    using dpichange_callback = std::function<void(window&, int dpi)>;            // new DPI
    void on_resize(resize_callback cb);
    void on_move(move_callback cb);
    void on_dpichange(dpichange_callback cb);

    // The OS window handle, or null before the window is shown.
    scl2::winhandle_t native_handle() const;

    // The shared implementation object. It is stable across window moves
    // (moving a window hands this shared_ptr over), so attached helpers can
    // keep a copy and stay valid no matter how the facade is moved.
    std::shared_ptr<impl> shared_impl() const { return m_impl; }

    // Register an extra handler for a window message not otherwise handled
    // (e.g. a tray icon's callback message). Invoked on the worker thread;
    // return true to mark the message handled (skips DefWindowProc). Pass a
    // null hook to unregister. Param types match WPARAM / LPARAM.
    using message_hook = std::function<bool(std::uintptr_t wParam, std::intptr_t lParam)>;
    void set_message_hook(unsigned int message, message_hook hook);

    // Adds a child control; the window assigns it a unique control ID and
    // owns it for as long as the window is alive. Controls are actually
    // created later by generate_children() when the OS window is made.
    void add_child(std::shared_ptr<control> child);

    // Creates the OS windows for all added children. Called by the window
    // procedure on WM_CREATE (hence public: the free-function wndproc cannot
    // be a friend without pulling windows.h into this header). Normally not
    // invoked by user code.
    void generate_children(scl2::winhandle_t parent_hwnd);

    // Moves all child controls to their DPI-scaled positions. Called on
    // WM_CREATE (after generate_children) and WM_DPICHANGED.
    void relayout_children(scl2::winhandle_t parent_hwnd);

    // Re-sends the (current) window font to every child via WM_SETFONT. Called
    // by the window procedure on WM_DPICHANGED after recreating the font.
    void apply_font_to_children();

    // Dispatches a WM_COMMAND received from a child control to that control's
    // event handlers (cid = LOWORD, notification = HIWORD). Called by the
    // window procedure; not for user code.
    void dispatch_command(control_id_t cid, unsigned int notification);

private:
    // control IDs only need to be unique within this window; start above the
    // system-reserved IDs (IDOK..IDHELP = 1..9) and typical low menu IDs.
    control_id_t alloc_control_id();

    std::shared_ptr<impl> m_impl;
};

// A window meant for background / tray-only programs: it owns a hidden
// message-pump window (the worker thread + OS window are created in the
// constructor via start()) but it is never shown. Keep it alive for as long
// as the background work must run; its destructor joins the worker thread.
// If you ever do want it visible, show() is not disabled and works as usual.
class SCF_EXPORT dummyWindow : public window {
public:
    dummyWindow()
        : window(scl2::Rect{ 0, 0, 200, 100 }) { start(); }
    dummyWindow(scl2::Geometry geometry)
        : window(geometry) { start(); }
    dummyWindow(scl2::Geometry geometry, const std::wstring& title)
        : window(geometry, title) { start(); }
    dummyWindow(scl2::Geometry geometry, const std::wstring& title, WindowFlags flags)
        : window(geometry, title, flags) { start(); }

    dummyWindow(const dummyWindow&) = delete;
    dummyWindow& operator=(const dummyWindow&) = delete;
    dummyWindow(dummyWindow&&) = default;
    dummyWindow& operator=(dummyWindow&&) = default;
};

// Shows a window sized to the bitmap's logical dimensions, the bitmap is
// drawn with fit_into(Contain). For small bitmaps (e.g. an unscaled QR code)
// pass a `scale` (>= 1): the window is then bitmap size x scale.
SCF_EXPORT window showBitmap(const scl2::bitmap_1c& bmp);
SCF_EXPORT window showBitmap(const scl2::bitmap_1c& bmp, int scale);
SCF_EXPORT window showBitmap(const scl2::bitmap_1c& bmp, int scale, const std::string& title);
SCF_EXPORT window showBitmap(const scl2::bitmap_1c& bmp, const std::string& title);

// Color-bitmap overloads (8/24/32-bit, loaded via bitmap<rgba8>::fromBmp).
SCF_EXPORT window showBitmap(const scl2::bitmap<scl2::rgba8>& bmp);
SCF_EXPORT window showBitmap(const scl2::bitmap<scl2::rgba8>& bmp, int scale);
SCF_EXPORT window showBitmap(const scl2::bitmap<scl2::rgba8>& bmp, int scale, const std::string& title);
SCF_EXPORT window showBitmap(const scl2::bitmap<scl2::rgba8>& bmp, const std::string& title);



enum class DialogButton : int {
    Closed = 0, // Window get closed by user.
    Ok, Cancel, Yes, No, Retry, Abort, Ignore,

    // We provide up to 5 customizable buttons.
    // You will be able to set the text of these buttons before showing the dialog.
    CustomName1, CustomName2, CustomName3, CustomName4, CustomName5
};

// Dialog window that delivers a typed result through get() once the window
// closes. The result is produced by the provider bound via bind_result()
// (done by the factory functions below) on the worker thread.
template <typename T>
class SCF_EXPORT dialog : public window {
public:
    dialog(scl2::Geometry geometry, const std::wstring& title)
        : window(geometry, title) {}

    // Blocks until the window closes and returns the dialog result.
    T get() { return m_future.get(); }

    // Bind the result provider: it runs on the worker thread when the window
    // closes; its return value is delivered by get().
    void bind_result(std::function<T(window&)> provider) {
        auto pr = std::make_shared<std::promise<T>>();
        m_future = pr->get_future();
        on_close([pr, provider = std::move(provider)](window& w) mutable {
            try {
                pr->set_value(provider(w));
            } catch (...) {
                pr->set_exception(std::current_exception());
            }
        });
    }

private:
    std::future<T> m_future;
};

// The concrete instantiations are defined and exported in scf.cpp. For
// consumers these extern declarations suppress local instantiation so they
// link against the DLL's versions. They are skipped when building the DLL
// itself: scf.cpp instantiates them right here, and `extern` combined with a
// dllexport class would be rejected (warning C4910).
#if !defined(SCF_CPP)
extern template class dialog<DialogButton>;
extern template class dialog<std::wstring>;
extern template class dialog<long long>;
extern template class dialog<long double>;
extern template class dialog<bool>;
#endif

// Show a confirmation dialog; the result identifies the button used to close
// the window (DialogButton::Closed if the window was closed directly).
// The wstring overload is the core implementation (no encoding conversion);
// the string one is a UTF-8 convenience wrapper.
SCF_EXPORT dialog<DialogButton> askForConfirmation(const std::wstring& content, const std::wstring& title);
SCF_EXPORT dialog<DialogButton> askForConfirmation(const scl2::string& content, const scl2::string& title);


// Show an "About SCF" dialog, which contains the version, author, pages and license information of SCF.
SCF_EXPORT window aboutSCF();

} // namespace scf

