/*  scf_internal.hpp -- shared implementation details for the scf library.
    NOT installed; only used when building the library itself (scf.cpp,
    scf_tray.cpp). Consumers only ever see the incomplete `window::impl`.  */

#pragma once

#include "scf.hpp"

#include <exception>
#include <mutex>
#include <condition_variable>

#include <SharedCppLib2/platform.hpp>
#include <commctrl.h>

namespace scf {

// Everything the worker thread touches lives here. Because it is shared via
// shared_ptr, a moved `window` simply re-points `owner` and the running
// thread stays valid (it never holds a raw pointer to the facade object).
// Sharing this header between scf.cpp and scf_tray.cpp lets attached helpers
// (e.g. tray_icon) hold a shared_ptr<impl> that stays valid across facade
// moves -- a `window&`/`window*` would dangle once the facade is moved.
struct window::impl {
    scl2::Geometry geometry;   // set before the worker starts, then read-only
    bool geometry_is_client = true; // geometry = client size; non-client added at create
    std::wstring title;

    // guarded by mtx (written by the facade thread, read by the worker)
    window* owner = nullptr;
    std::function<void(window&)> render_cb;
    std::function<void(window&)> close_cb;
    std::function<void(window&, int, int)> resize_cb;
    std::function<void(window&, int, int)> move_cb;
    std::function<void(window&, int)> dpichange_cb;

    // set before the worker starts, then read-only on the worker
    WindowFlags flags = WindowFlags::None;
    scl2::Rect drag_zone{0, 0, 0, 0};   // logical client rect; zero = off
    bool keep_aspect_ratio = false;

    // worker-thread only: aspect-ratio bookkeeping during interactive sizing
    double aspect = 0.0;    // current client width/height ratio
    bool in_sizemove = false;

    // Resources that are used in the window lifetime.
    HFONT hFont = nullptr;

    // Controls owned by this window. A control is an independent class (not a
    // window subclass); the list keeps them alive as long as this window is.
    // Managed through window member functions.
    std::map<control_id_t, std::shared_ptr<control>> children;

    // worker-thread only: valid HDC while handling WM_PAINT
    HDC hdc = nullptr;

    // worker-thread only: cached packed DIB rows of the last painted source,
    // reused across repaints so a large bitmap is not re-packed on every
    // resize tick. Rebuilt when the source size changes.
    std::shared_ptr<const scl2::bytearray> dib_payload;
    LONG dib_src_w = 0;
    LONG dib_src_h = 0;

    // worker-thread only: cached BGRA rows for color (rgba8) bitmaps.
    std::shared_ptr<const scl2::bytearray> dib_color_payload;
    LONG dib_color_src_w = 0;
    LONG dib_color_src_h = 0;

    // handshake for the lazy start of the worker thread
    std::mutex mtx;
    std::condition_variable cv;
    bool started = false;
    bool window_ready = false;
    bool window_failed = false;
    std::exception_ptr create_error;   // original exception from the worker

    // atomics: read/written from both threads
    std::atomic<bool> close_requested{false};
    std::atomic<HWND> hwnd{nullptr};
    // control IDs only need to be unique within this window; start above the
    // system-reserved IDs (IDOK..IDHELP = 1..9) and typical low menu IDs.
    std::atomic<UINT> next_control_id{0x0100};

    // guarded by mtx: extra handlers for otherwise-unhandled window messages
    std::map<UINT, std::function<bool(std::uintptr_t, std::intptr_t)>> message_hooks;

    std::thread worker;

    void ensure_started();   // start the worker and wait for the window
    void post(UINT msg);     // thread-safe request to the window
    HWND create_window();    // must run on the worker thread
    void run();              // worker thread entry
};

} // namespace scf
