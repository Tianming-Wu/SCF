#include "scf.hpp"

#include "scf_internal.hpp"   // defines window::impl (shared with scf_tray)

#include <atomic>
#include <algorithm>
#include <cmath>
#include <exception>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <vector>

#include <SharedCppLib2/platform.hpp>
#include <SharedCppLib2/string.hpp>

#include <commctrl.h>

// Wrap a programmatic control write (SetWindowTextW / SendMessageW / ...) so
// the change notifications it triggers are suppressed in handle_event().
// Exception-safe: the flag is always restored even if `expr` throws.
#define SYSCHANGE(expr)                                                       \
    do {                                                                      \
        m_suppress_events.store(true);                                        \
        try { expr; } catch (...) { m_suppress_events.store(false); throw; }  \
        m_suppress_events.store(false);                                       \
    } while (0)

namespace scf {

// custom window messages posted from other threads
enum : UINT {
    WM_SCF_SHOW = WM_APP + 1,
    WM_SCF_HIDE = WM_APP + 2,
    WM_SCF_RESIZE = WM_APP + 3,  // lParam packs logical client w (lo32) / h (hi32)
    WM_SCF_CENTER = WM_APP + 4,
};

// Custom notification code for textbox::on_enter, sent as HIWORD of a
// WM_COMMAND by the edit's window subclass (above the standard EN_* range).
constexpr uint16_t SCF_EN_ENTER = 0x7000;

// SCF's default UI font (Segoe UI). The height is scaled to `dpi` with MulDiv
// and passed as a POSITIVE lfHeight: negative heights get re-scaled by GDI per
// the DC's DPI (which double-scales if pre-scaled), positive ones do not -- so
// on a DPI change we recreate the font with the new DPI and re-apply it.
// baseHeight is the logical (96-DPI) pixel size.
HFONT generateFont(int baseHeight = 19, UINT dpi = 0)
{
    if (dpi == 0) dpi = GetDpiForSystem();
    LOGFONTA lf = {0};
    lf.lfHeight = ::MulDiv(baseHeight, static_cast<int>(dpi), 96); // positive physical px
    lf.lfWeight = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    strncpy_s(lf.lfFaceName, "Segoe UI", LF_FACESIZE - 1);

    HFONT hFont = CreateFontIndirectA(&lf);
    return hFont;
}

// Measure `text` rendered in `font` (GDI text layout). If `max_width` > 0 the
// text is word-wrapped at that width (CJK breaks per character) and the height
// grows to the wrapped line count; otherwise a single line is measured and the
// width is the text's natural width. Results are in pixels at 100% scale.
static scl2::Size measure_text(HFONT font, const std::wstring& text, int max_width)
{
    HDC hdc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(hdc, font);
    RECT rc{0, 0, max_width > 0 ? max_width : 10000, 0};
    DWORD fmt = DT_CALCRECT | DT_NOPREFIX | DT_LEFT;
    if (max_width > 0) fmt |= DT_WORDBREAK;
    DrawTextW(hdc, text.c_str(), -1, &rc, fmt);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    return scl2::Size(rc.right - rc.left, rc.bottom - rc.top);
}

LRESULT CALLBACK scfWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// The window style implied by `flags`. create_window() and the non-client-area
// math in WM_SIZING / WM_SCF_RESIZE all have to agree on this: if one of them
// assumed a resizable frame while the window was created without one, the
// client-size calculation would be off by the border width.
static DWORD window_style_from_flags(WindowFlags flags)
{
    // Frameless windows are WS_POPUP (no caption/borders); everything else
    // gets the normal overlapped chrome.
    if ((flags & WindowFlags::Frameless) != WindowFlags::None)
        return WS_POPUP;

    DWORD style = WS_OVERLAPPEDWINDOW;
    if ((flags & WindowFlags::FixedSize) != WindowFlags::None)
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    return style;
}

window::window()
    : m_impl(std::make_shared<impl>())
{
    m_impl->owner = this;
}

window::window(scl2::Geometry geometry)
    : m_impl(std::make_shared<impl>())
{
    m_impl->geometry = geometry;
    m_impl->owner = this;
}

window::window(scl2::Geometry geometry, const std::wstring& title)
    : m_impl(std::make_shared<impl>())
{
    m_impl->geometry = geometry;
    m_impl->title = title;
    m_impl->owner = this;
}

window::window(scl2::Geometry geometry, const std::wstring& title, WindowFlags flags)
    : m_impl(std::make_shared<impl>())
{
    m_impl->geometry = geometry;
    m_impl->title = title;
    m_impl->flags = flags;
    m_impl->owner = this;
}

window::~window()
{
    if (!m_impl) return;

    // The worker thread can only reach the facade through `owner` (guarded by
    // mtx); clear it first so callbacks never touch a dying object, then ask
    // the window to go away and wait for the worker to exit.
    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        m_impl->owner = nullptr;
    }
    m_impl->close_requested.store(true);
    if (HWND h = m_impl->hwnd.load()) {
        PostMessageW(h, WM_CLOSE, 0, 0);
    }
    if (m_impl->worker.joinable()) {
        m_impl->worker.join();
    }
}

window::window(window&& other) noexcept
    : m_impl(std::move(other.m_impl))
{
    if (m_impl) {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        m_impl->owner = this;
        for (auto& [cid, child] : m_impl->children) {
            if (child) child->m_owner = this;
        }
    }
}

window& window::operator=(window&& other) noexcept
{
    if (this == &other) return *this;

    if (m_impl) {
        {
            std::lock_guard<std::mutex> lk(m_impl->mtx);
            m_impl->owner = nullptr;
        }
        m_impl->close_requested.store(true);
        if (HWND h = m_impl->hwnd.load()) PostMessageW(h, WM_CLOSE, 0, 0);
        if (m_impl->worker.joinable()) m_impl->worker.join();
        m_impl.reset();
    }

    m_impl = std::move(other.m_impl);
    if (m_impl) {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        m_impl->owner = this;
        for (auto& [cid, child] : m_impl->children) {
            if (child) child->m_owner = this;
        }
    }
    return *this;
}

void window::start()
{
    if (!m_impl) return;
    m_impl->ensure_started();   // may throw if the window could not be made
}

void window::show()
{
    if (!m_impl) return;
    start();                    // create the worker + OS window if not yet
    m_impl->post(WM_SCF_SHOW);  // then reveal it
}

void window::hide()
{
    if (!m_impl) return;
    m_impl->post(WM_SCF_HIDE);
}

void window::close()
{
    if (!m_impl) return;
    m_impl->close_requested.store(true);
    if (HWND h = m_impl->hwnd.load()) {
        PostMessageW(h, WM_CLOSE, 0, 0);
    }
}

void window::wait_for_closed()
{
    if (!m_impl) return;
    // Block the caller until the worker thread (message loop) has exited. If
    // the window is still open, call close() first or let the user close it.
    // Guard against self-join in case this is called from a callback.
    if (m_impl->worker.joinable() &&
        m_impl->worker.get_id() != std::this_thread::get_id()) {
        m_impl->worker.join();
    }
}

void window::request_render()
{
    if (!m_impl) return;
    m_impl->post(WM_PAINT);
}

void window::on_render(render_callback cb)
{
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->render_cb = std::move(cb);
}

void window::on_close(close_callback cb)
{
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->close_cb = std::move(cb);
}

void window::use_client_geometry(bool on)
{
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->geometry_is_client = on;
}

void window::set_drag_zone(const scl2::Rect& zone)
{
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->drag_zone = zone;
}

void window::resize(int width, int height)
{
    if (!m_impl) return;
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (HWND h = m_impl->hwnd.load()) {
        // Resize on the worker thread (cross-thread SetWindowPos is unsafe).
        // lParam packs the logical client w (lo32) / h (hi32); x64 LPARAM is 64-bit.
        const LPARAM packed = (static_cast<LPARAM>(width) & 0xFFFFFFFF)
                            | (static_cast<LPARAM>(height) << 32);
        PostMessageW(h, WM_SCF_RESIZE, 0, packed);
    } else {
        // Not shown yet: remember the size for creation.
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        m_impl->geometry.w = width;
        m_impl->geometry.h = height;
    }
}

void window::center_window()
{
    if (!m_impl) return;
    if (HWND h = m_impl->hwnd.load()) PostMessageW(h, WM_SCF_CENTER, 0, 0);
    // Before show(): creation already centers the window.
}

void window::set_keep_aspect_ratio(bool on)
{
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->keep_aspect_ratio = on;
}

void window::set_fixed_size(bool on)
{
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    // The flag is read by window_style_from_flags() when the OS window is
    // created, so this has to happen before the worker starts (i.e. before
    // show()/start()); changing it later would not rebuild the frame.
    const auto bit = static_cast<uint32_t>(WindowFlags::FixedSize);
    auto f = static_cast<uint32_t>(m_impl->flags);
    if (on) f |= bit;
    else    f &= ~bit;
    m_impl->flags = static_cast<WindowFlags>(f);
}

void window::on_resize(resize_callback cb)
{
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->resize_cb = std::move(cb);
}

void window::on_move(move_callback cb)
{
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->move_cb = std::move(cb);
}

void window::on_dpichange(dpichange_callback cb)
{
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->dpichange_cb = std::move(cb);
}

scl2::winhandle_t window::native_handle() const
{
    return m_impl ? scl2::from_handle(m_impl->hwnd.load()) : nullptr;
}

void window::set_message_hook(unsigned int message, message_hook hook)
{
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    if (hook) m_impl->message_hooks[message] = std::move(hook);
    else m_impl->message_hooks.erase(message);
}

control_id_t window::alloc_control_id()
{
    if (!m_impl) return 0;
    return m_impl->next_control_id.fetch_add(1);
}

void window::generate_children(scl2::winhandle_t parent_hwnd)
{
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);

    for (auto& [cid, child] : m_impl->children) {
        if (child) {
            child->create_control(cid, parent_hwnd);
            // Give every child the window font (Segoe UI) — the default system
            // font looks bad on HiDPI.
            if (m_impl->hFont && child->m_hwnd) {
                SendMessageW(scl2::to_handle<HWND>(child->m_hwnd), WM_SETFONT,
                             reinterpret_cast<WPARAM>(m_impl->hFont), TRUE);
            }
        }
    }
}

void window::relayout_children(scl2::winhandle_t parent_hwnd)
{
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);

    const UINT dpi = GetDpiForWindow(scl2::to_handle<HWND>(parent_hwnd));
    const double factor = static_cast<double>(dpi) / 96.0;

    for (auto& [cid, child] : m_impl->children) {
        if (child && child->m_hwnd) {
            const scl2::Rect phys = child->m_geometry.scale(factor);
            SetWindowPos(scl2::to_handle<HWND>(child->m_hwnd), nullptr,
                         phys.x, phys.y, phys.w, phys.h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

void window::apply_font_to_children()
{
    if (!m_impl || !m_impl->hFont) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    for (auto& [cid, child] : m_impl->children) {
        if (child && child->m_hwnd) {
            SendMessageW(scl2::to_handle<HWND>(child->m_hwnd), WM_SETFONT,
                         reinterpret_cast<WPARAM>(m_impl->hFont), TRUE);
        }
    }
}

void window::dispatch_command(control_id_t cid, unsigned int notification)
{
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);

    auto it = m_impl->children.find(cid);
    if (it != m_impl->children.end() && it->second) {
        it->second->handle_event(notification);
    }
}

void window::add_child(std::shared_ptr<control> child)
{
    if (!m_impl || !child) return;
    std::lock_guard<std::mutex> lk(m_impl->mtx);

    control_id_t new_cid = alloc_control_id();
    m_impl->children[new_cid] = child;
    child->m_owner = this;   // lets the control reach its window (close_owner)

    // A container control (e.g. radiogroup) also registers its sub-controls
    // as window children so they get created/dispatched like any other child.
    std::vector<std::shared_ptr<control>> subs;
    child->collect_sub_controls(subs);
    for (auto& sub : subs) {
        if (!sub) continue;
        control_id_t cid = alloc_control_id();
        m_impl->children[cid] = sub;
        sub->m_owner = this;
    }

    // We do not set the id now. It will be set later in generate_children().
}

void window::paint_bitmap(const scl2::bitmap_1c& bmp)
{
    if (!m_impl) return;

    // Only valid while handling WM_PAINT on the worker thread: `hdc` is set
    // by the window procedure between BeginPaint/EndPaint.
    HDC hdc = m_impl->hdc;
    if (!hdc) return;

    const LONG bw = static_cast<LONG>(bmp.width());
    const LONG bh = static_cast<LONG>(bmp.height());
    if (bw <= 0 || bh <= 0) return;

    // Cache the packed source rows. Repacking a large bitmap on every
    // repaint is wasted work when the source is unchanged.
    if (!m_impl->dib_payload || m_impl->dib_src_w != bw || m_impl->dib_src_h != bh) {
        m_impl->dib_payload = std::make_shared<scl2::bytearray>(bmp.toByteArrayPadded());
        m_impl->dib_src_w = bw;
        m_impl->dib_src_h = bh;
    }

    RECT rc;
    GetClientRect(m_impl->hwnd.load(), &rc);
    const LONG cw = rc.right - rc.left;
    const LONG ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    // Letterbox background (Contain leaves margins on one axis).
    HBRUSH white = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    FillRect(hdc, &rc, white);

    // Contain: scale to fit the client area preserving aspect ratio, centered.
    // Scaling is left to StretchDIBits (fast system path); we never resample
    // the bitmap on the CPU per repaint, which is what made resize stutter.
    const double scale = (std::min)(static_cast<double>(cw) / bw,
                                    static_cast<double>(ch) / bh);
    const LONG dw = (std::max)(1L, static_cast<LONG>(bw * scale));
    const LONG dh = (std::max)(1L, static_cast<LONG>(bh * scale));
    const LONG dx = (cw - dw) / 2;
    const LONG dy = (ch - dh) / 2;

    // BITMAPINFO declares only a 1-entry palette, but we need 2; use an
    // extended stack struct so writing bmiColors[1] cannot overflow the frame.
    struct {
        BITMAPINFOHEADER bmiHeader;
        RGBQUAD bmiColors[2];
    } bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bw;
    bi.bmiHeader.biHeight = -bh;   // top-down: first row = top of image
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 1;
    // palette: 0 -> white, 1 -> black (foreground)
    bi.bmiColors[0].rgbRed = bi.bmiColors[0].rgbGreen = bi.bmiColors[0].rgbBlue = 255;
    bi.bmiColors[1].rgbRed = bi.bmiColors[1].rgbGreen = bi.bmiColors[1].rgbBlue = 0;

    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc, dx, dy, dw, dh, 0, 0, bw, bh,
                  reinterpret_cast<const void*>(m_impl->dib_payload->data()),
                  reinterpret_cast<const BITMAPINFO*>(&bi), DIB_RGB_COLORS, SRCCOPY);
}

void window::paint_bitmap(const scl2::bitmap<scl2::rgba8>& bmp)
{
    if (!m_impl) return;

    // Only valid while handling WM_PAINT on the worker thread: `hdc` is set
    // by the window procedure between BeginPaint/EndPaint.
    HDC hdc = m_impl->hdc;
    if (!hdc) return;

    const LONG bw = static_cast<LONG>(bmp.width());
    const LONG bh = static_cast<LONG>(bmp.height());
    if (bw <= 0 || bh <= 0) return;

    // Cache the packed source rows. GDI's 32-bit DIB is BGRA while rgba8 is
    // RGBA, so swap r/b while packing. Rebuilt only when the source changes.
    if (!m_impl->dib_color_payload || m_impl->dib_color_src_w != bw || m_impl->dib_color_src_h != bh) {
        auto payload = std::make_shared<scl2::bytearray>(
            static_cast<size_t>(bw) * static_cast<size_t>(bh) * 4);
        std::byte* out = payload->data();
        size_t i = 0;
        for (LONG y = 0; y < bh; ++y) {
            for (LONG x = 0; x < bw; ++x) {
                const auto px = bmp.getPixel(static_cast<size_t>(x), static_cast<size_t>(y));
                out[i++] = static_cast<std::byte>(px.b);
                out[i++] = static_cast<std::byte>(px.g);
                out[i++] = static_cast<std::byte>(px.r);
                out[i++] = static_cast<std::byte>(px.a);
            }
        }
        m_impl->dib_color_payload = std::move(payload);
        m_impl->dib_color_src_w = bw;
        m_impl->dib_color_src_h = bh;
    }

    RECT rc;
    GetClientRect(m_impl->hwnd.load(), &rc);
    const LONG cw = rc.right - rc.left;
    const LONG ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    // Letterbox background (Contain leaves margins on one axis).
    HBRUSH white = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    FillRect(hdc, &rc, white);

    // Contain: scale to fit the client area preserving aspect ratio, centered.
    const double scale = (std::min)(static_cast<double>(cw) / bw,
                                    static_cast<double>(ch) / bh);
    const LONG dw = (std::max)(1L, static_cast<LONG>(bw * scale));
    const LONG dh = (std::max)(1L, static_cast<LONG>(bh * scale));
    const LONG dx = (cw - dw) / 2;
    const LONG dy = (ch - dh) / 2;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bw;
    bi.bmiHeader.biHeight = -bh;   // top-down: first row = top of image
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc, dx, dy, dw, dh, 0, 0, bw, bh,
                  reinterpret_cast<const void*>(m_impl->dib_color_payload->data()),
                  &bi, DIB_RGB_COLORS, SRCCOPY);
}

void window::impl::ensure_started()
{
    {
        std::unique_lock<std::mutex> lk(mtx);
        if (started) return;
        started = true;
    }

    worker = std::thread(&window::impl::run, this);

    std::unique_lock<std::mutex> lk(mtx);
    cv.wait(lk, [this] { return window_ready || window_failed; });
    if (window_failed) {
        if (worker.joinable()) worker.join();
        // Re-throw the original exception (with its message) from the worker.
        if (create_error) std::rethrow_exception(create_error);
        throw std::runtime_error("Failed to create window");
    }
}

void window::impl::post(UINT msg)
{
    if (HWND h = hwnd.load()) {
        PostMessageW(h, msg, 0, 0);
    }
}

void window::impl::run()
{
    HWND h;
    try {
        h = create_window();
    } catch (...) {
        std::lock_guard<std::mutex> lk(mtx);
        create_error = std::current_exception();
        window_failed = true;
        cv.notify_all();
        return;
    }

    hwnd.store(h);
    {
        std::lock_guard<std::mutex> lk(mtx);
        window_ready = true;
    }
    cv.notify_all();

    // The window may have been asked to close before it was even created.
    if (close_requested.load()) {
        DestroyWindow(h);   // -> WM_DESTROY -> PostQuitMessage -> loop exits
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

HWND window::impl::create_window()
{
    // Per-monitor V2 DPI awareness (once per process): the window then works
    // in physical pixels and receives WM_DPICHANGED when moved across monitors.
    static bool dpiAware = [] {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return true;
    }();
    (void)dpiAware;

    // Register the ComCtl32 control classes once. Harmless for the plain
    // user32 controls (BUTTON/EDIT/STATIC), needed for ListView/TreeView etc.
    static bool comctl = [] {
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);
        return true;
    }();
    (void)comctl;

    static HINSTANCE hInstance = GetModuleHandleW(nullptr);

    static bool classReg = []{
        
        WNDCLASSEXW wc = { 0 };
        wc.cbSize = sizeof(wc);

        wc.cbClsExtra = 0;
        wc.cbWndExtra = sizeof(LONG_PTR);   // room for GWLP_USERDATA
        wc.hInstance = hInstance;
        wc.hIcon = nullptr;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszMenuName = nullptr;
        wc.lpszClassName = L"SCFWindowClass";

        wc.lpfnWndProc = scfWndProc;

        RegisterClassExW(&wc);

        return true;
    }();

    // The stored geometry is logical (100% scale); convert it to physical
    // pixels for the current DPI before handing it to the system. By default
    // the geometry is the CLIENT-area size: the non-client area (caption +
    // borders, which vary per system) is added here via AdjustWindowRectEx.
    // Callers that already pass a raw outer size opt out via
    // use_client_geometry(false).
    const UINT dpi = GetDpiForSystem();
    const double factor = static_cast<double>(dpi) / 96.0;
    const scl2::Rect phys = geometry.scale(factor);

    // Frameless windows are WS_POPUP (no caption/borders); everything else
    // gets the normal overlapped chrome (minus the resize frame when the
    // FixedSize flag is set).
    const DWORD style = window_style_from_flags(flags);

    RECT rc{0, 0, phys.w, phys.h};
    int ncx_left = 0, ncx_top = 0;   // non-client insets, for client-area centering
    if (geometry_is_client) {
        AdjustWindowRectEx(&rc, style, FALSE, 0);
        ncx_left = -rc.left;
        ncx_top = -rc.top;
    }
    const LONG win_w = rc.right - rc.left;
    const LONG win_h = rc.bottom - rc.top;

    // Center on the primary monitor's work area (physical pixels) unless the
    // caller already chose an explicit position. When the geometry is a
    // client size, center the CLIENT area exactly (the caption sits above it,
    // so centering the outer rect would push the client off-center); when the
    // geometry is a raw outer size, center the outer rect as before.
    int x = phys.x;
    int y = phys.y;
    if (x == 0 && y == 0) {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        if (geometry_is_client) {
            x = work.left + (work.right - work.left - phys.w) / 2 - ncx_left;
            y = work.top + (work.bottom - work.top - phys.h) / 2 - ncx_top;
        } else {
            x = work.left + (work.right - work.left - win_w) / 2;
            y = work.top + (work.bottom - work.top - win_h) / 2;
        }
        if (x < 0) x = 0;   // window larger than the work area
        if (y < 0) y = 0;
    }

    HWND hwnd = CreateWindowExW(
        0,
        L"SCFWindowClass",
        title.empty() ? L"SCF" : title.c_str(),
        style,
        x, y, win_w, win_h,
        nullptr, nullptr, hInstance, this);

    if (!hwnd) {
        throw std::runtime_error("Failed to create window: " +
                                 platform::windows::TranslateLastError());
    }
    return hwnd;
}

LRESULT CALLBACK scfWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        // Must forward to DefWindowProcW: it initializes the non-client area,
        // including reading the title from lpWindowName. Returning TRUE without
        // it leaves the caption bar empty.
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
    case WM_CREATE: {
        // Create the child controls here. The window handle exists but is not
        // stored in impl yet (CreateWindowExW hasn't returned), so the parent
        // handle must come from this message's hwnd parameter.
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p) {
            // Load the window font first so generate_children can apply it.
            // Scale it to the window's DPI (positive height, see generateFont).
            if (!p->hFont) p->hFont = generateFont(19, GetDpiForWindow(hwnd));
            if (p->owner) {
                p->owner->generate_children(scl2::from_handle(hwnd));
                p->owner->relayout_children(scl2::from_handle(hwnd));
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        // Child controls send WM_COMMAND: LOWORD(wParam) = control id,
        // HIWORD(wParam) = notification code (e.g. BN_CLICKED).
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p && p->owner) {
            p->owner->dispatch_command(
                static_cast<control_id_t>(LOWORD(wParam)),
                static_cast<unsigned int>(HIWORD(wParam)));
        }
        return 0;
    }
    case WM_DESTROY: {
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p && p->hFont) {
            DeleteObject(p->hFont);
            p->hFont = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    case WM_CLOSE: {
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        std::function<void(window&)> cb;
        window* owner = nullptr;
        if (p) {
            std::lock_guard<std::mutex> lk(p->mtx);
            cb = p->close_cb;
            owner = p->owner;
        }
        if (cb && owner) cb(*owner);
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_SIZE: {
        // Windows only invalidates the newly exposed parts of the client area
        // on a resize; force a full repaint so the bitmap is redrawn cleanly.
        InvalidateRect(hwnd, nullptr, TRUE);
        // Notify with the logical (100% scale) client size.
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        std::function<void(window&, int, int)> cb;
        window* owner = nullptr;
        if (p) {
            const UINT dpi = GetDpiForWindow(hwnd);
            const int lw = (static_cast<int>(LOWORD(lParam)) * 96 + static_cast<int>(dpi) / 2) / static_cast<int>(dpi);
            const int lh = (static_cast<int>(HIWORD(lParam)) * 96 + static_cast<int>(dpi) / 2) / static_cast<int>(dpi);
            // Base ratio for keepAspectRatio; frozen while a drag is in progress.
            if (!p->in_sizemove && lh > 0) p->aspect = static_cast<double>(lw) / lh;
            {
                std::lock_guard<std::mutex> lk(p->mtx);
                cb = p->resize_cb;
                owner = p->owner;
            }
            if (cb && owner) cb(*owner, lw, lh);
        }
        return 0;
    }
    case WM_MOVE: {
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        std::function<void(window&, int, int)> cb;
        window* owner = nullptr;
        if (p) {
            {
                std::lock_guard<std::mutex> lk(p->mtx);
                cb = p->move_cb;
                owner = p->owner;
            }
            if (cb && owner) {
                cb(*owner,
                   static_cast<int>(static_cast<short>(LOWORD(lParam))),
                   static_cast<int>(static_cast<short>(HIWORD(lParam))));
            }
        }
        return 0;
    }
    case WM_ENTERSIZEMOVE: {
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p) p->in_sizemove = true;
        return 0;
    }
    case WM_EXITSIZEMOVE: {
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p) p->in_sizemove = false;
        return 0;
    }
    case WM_SIZING: {
        // keepAspectRatio: constrain the proposed rect to the client aspect
        // ratio while the user drags a border. Only fires during interactive
        // sizing, so resize() is unaffected.
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p && p->keep_aspect_ratio && p->in_sizemove && p->aspect > 0.0) {
            RECT* prc = reinterpret_cast<RECT*>(lParam);
            RECT ncx{0, 0, 0, 0};
            const DWORD style = window_style_from_flags(p->flags);
            AdjustWindowRectEx(&ncx, style, FALSE, 0);
            const int ins_l = -ncx.left, ins_t = -ncx.top, ins_r = ncx.right, ins_b = ncx.bottom;

            LONG cw = (prc->right - prc->left) - ins_l - ins_r;
            LONG ch = (prc->bottom - prc->top) - ins_t - ins_b;
            if (cw < 1) cw = 1;
            if (ch < 1) ch = 1;

            const bool left = wParam == WMSZ_LEFT || wParam == WMSZ_TOPLEFT || wParam == WMSZ_BOTTOMLEFT;
            const bool top  = wParam == WMSZ_TOP  || wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT;
            const bool vertical_only = (wParam == WMSZ_TOP || wParam == WMSZ_BOTTOM);

            if (vertical_only) cw = static_cast<LONG>(std::lround(static_cast<double>(ch) * p->aspect));
            else               ch = static_cast<LONG>(std::lround(static_cast<double>(cw) / p->aspect));

            if (left) prc->left   = prc->right - ins_l - cw - ins_r;
            else      prc->right  = prc->left  + ins_l + cw + ins_r;
            if (top)  prc->top    = prc->bottom - ins_t - ch - ins_b;
            else      prc->bottom = prc->top    + ins_t + ch + ins_b;
        }
        return 0;
    }
    case WM_DPICHANGED: {
        // wParam packs the new DPI; lParam points to the suggested new window
        // rect for that DPI -- apply it, then repaint at the new scale.
        const UINT dpi = static_cast<UINT>(LOWORD(wParam));
        const RECT* prc = reinterpret_cast<const RECT*>(lParam);
        if (prc) {
            SetWindowPos(hwnd, nullptr, prc->left, prc->top,
                         prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        // Recreate the font for the new DPI and re-apply it to every child so
        // text scales with the monitor (a fixed HFONT would not follow).
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p && p->owner) {
            HFONT old = p->hFont;
            p->hFont = generateFont(19, dpi);
            p->owner->apply_font_to_children();   // uses the new p->hFont
            if (old) DeleteObject(old);
            p->owner->relayout_children(scl2::from_handle(hwnd));
            std::function<void(window&, int)> cb;
            {
                std::lock_guard<std::mutex> lk(p->mtx);
                cb = p->dpichange_cb;
            }
            if (cb) cb(*p->owner, static_cast<int>(dpi));
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_RBUTTONUP: {
        // WindowFlags::rightClickExit: right-click closes the window.
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p && (p->flags & WindowFlags::rightClickExit) != WindowFlags::None) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
    case WM_LBUTTONDOWN: {
        // Frameless drag zone: a press inside it starts a window drag via the
        // system's caption handler (snap/Aero semantics included).
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p && p->drag_zone.w > 0 && p->drag_zone.h > 0) {
            const UINT dpi = GetDpiForWindow(hwnd);
            const double f = static_cast<double>(dpi) / 96.0;
            const scl2::Rect z = p->drag_zone.scale(f);
            const scl2::Point pt{
                static_cast<int>(static_cast<short>(LOWORD(lParam))),
                static_cast<int>(static_cast<short>(HIWORD(lParam))) };
            if (z.contains(pt)) {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
    case WM_CTLCOLORSTATIC: {
        // Labels (STATIC) must be transparent so they show the window's own
        // background; DefWindowProc would paint them with a slightly darker
        // system brush. NULL_BRUSH skips the control's own background fill
        // and TRANSPARENT text mode avoids a box behind the glyphs.
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        std::function<void(window&)> cb;
        window* owner = nullptr;
        if (p) {
            p->hdc = hdc;   // make the DC available to paint helpers
            {
                std::lock_guard<std::mutex> lk(p->mtx);
                cb = p->render_cb;
                owner = p->owner;
            }
        }
        if (cb && owner) cb(*owner);
        if (p) p->hdc = nullptr;
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SCF_SHOW:
        ShowWindow(hwnd, SW_SHOW);
        return 0;
    case WM_SCF_HIDE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_SCF_RESIZE: {
        // lParam packs the logical client size (lo32 = w, hi32 = h).
        const int cw = static_cast<int>(static_cast<LONG>(lParam & 0xFFFFFFFF));
        const int ch = static_cast<int>(static_cast<LONG>((lParam >> 32) & 0xFFFFFFFF));
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p) {
            const UINT dpi = GetDpiForWindow(hwnd);
            const int pw = (cw * static_cast<int>(dpi) + 48) / 96;   // logical -> physical
            const int ph = (ch * static_cast<int>(dpi) + 48) / 96;
            const DWORD style = window_style_from_flags(p->flags);
            RECT rc{0, 0, pw, ph};
            if (p->geometry_is_client) AdjustWindowRectEx(&rc, style, FALSE, 0);
            RECT wr;
            GetWindowRect(hwnd, &wr);
            SetWindowPos(hwnd, nullptr, wr.left, wr.top,
                         rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    }
    case WM_SCF_CENTER: {
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p) {
            RECT work{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            RECT wr;
            GetWindowRect(hwnd, &wr);
            int x, y;
            if (p->geometry_is_client) {
                POINT origin{0, 0};
                ClientToScreen(hwnd, &origin);
                const int ncx_l = origin.x - wr.left;
                const int ncx_t = origin.y - wr.top;
                RECT cr;
                GetClientRect(hwnd, &cr);
                x = work.left + (work.right - work.left - cr.right) / 2 - ncx_l;
                y = work.top + (work.bottom - work.top - cr.bottom) / 2 - ncx_t;
            } else {
                x = work.left + (work.right - work.left - (wr.right - wr.left)) / 2;
                y = work.top + (work.bottom - work.top - (wr.bottom - wr.top)) / 2;
            }
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    default: {
        // Let extra per-window message hooks (e.g. a tray icon callback)
        // handle otherwise-unhandled messages first.
        auto* p = reinterpret_cast<window::impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p) {
            std::function<bool(std::uintptr_t, std::intptr_t)> hook;
            {
                std::lock_guard<std::mutex> lk(p->mtx);
                auto it = p->message_hooks.find(uMsg);
                if (it != p->message_hooks.end()) hook = it->second;
            }
            if (hook && hook(static_cast<std::uintptr_t>(wParam), static_cast<std::intptr_t>(lParam))) {
                return 0;
            }
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
    }
}

// Textbox subclass: reports Enter presses in a single-line edit as a custom
// notification to the parent, which dispatches it to textbox::on_enter.
LRESULT CALLBACK scfEditSubclass(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
{
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
        const LONG style = static_cast<LONG>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        if (!(style & ES_MULTILINE)) {   // multiline edits use Enter for newline
            HWND parent = GetParent(hwnd);
            if (parent) {
                SendMessageW(parent, WM_COMMAND,
                             MAKEWPARAM(GetDlgCtrlID(hwnd), SCF_EN_ENTER),
                             reinterpret_cast<LPARAM>(hwnd));
            }
            return 0;
        }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

namespace {

// Shared window factory for any bitmap pixel type: sizes the window to
// bitmap x scale and paints it (Contain) on every repaint. The render lambda
// dispatches to the matching window::paint_bitmap overload.
template <typename Pixel>
window makeBitmapWindow(const scl2::bitmap<Pixel>& bmp, int scale, const std::wstring& title)
{
    // Keep our own copy so the renderer stays valid for the whole window
    // lifetime, no matter what the caller does with `bmp` afterwards.
    auto bmp_keep = std::make_shared<scl2::bitmap<Pixel>>(bmp);

    // (avoid std::max: windows.h defines a `max` macro)
    const int s = scale < 1 ? 1 : scale;
    const int w = static_cast<int>(bmp.width()) * s;
    const int h = static_cast<int>(bmp.height()) * s;

    window win({0, 0, w, h}, title);
    win.on_render([bmp_keep](window& self) {
        self.paint_bitmap(*bmp_keep);
    });
    win.show();
    return win;
}

} // namespace

window showBitmap(const scl2::bitmap_1c &bmp)
{
    return makeBitmapWindow(bmp, 1, L"");
}

window showBitmap(const scl2::bitmap_1c& bmp, int scale)
{
    return makeBitmapWindow(bmp, scale, L"");
}

window showBitmap(const scl2::bitmap_1c& bmp, int scale, const std::string& title)
{
    return makeBitmapWindow(bmp, scale, scl2::str_to_wstr(title));
}

window showBitmap(const scl2::bitmap_1c& bmp, const std::string& title)
{
    return makeBitmapWindow(bmp, 1, scl2::str_to_wstr(title));
}

window showBitmap(const scl2::bitmap<scl2::rgba8>& bmp)
{
    return makeBitmapWindow(bmp, 1, L"");
}

window showBitmap(const scl2::bitmap<scl2::rgba8>& bmp, int scale)
{
    return makeBitmapWindow(bmp, scale, L"");
}

window showBitmap(const scl2::bitmap<scl2::rgba8>& bmp, int scale, const std::string& title)
{
    return makeBitmapWindow(bmp, scale, scl2::str_to_wstr(title));
}

window showBitmap(const scl2::bitmap<scl2::rgba8>& bmp, const std::string& title)
{
    return makeBitmapWindow(bmp, 1, scl2::str_to_wstr(title));
}

// Concrete instantiations of dialog<T> live in this DLL (see the extern
// template declarations in scf.hpp).
template class SCF_EXPORT dialog<DialogButton>;
template class SCF_EXPORT dialog<std::wstring>;
template class SCF_EXPORT dialog<long long>;
template class SCF_EXPORT dialog<long double>;
template class SCF_EXPORT dialog<bool>;

// wstring version: the core implementation. Content/title are already Unicode,
// so CJK arrives intact regardless of the caller's console codepage.
dialog<DialogButton> askForConfirmation(const std::wstring& content, const std::wstring& title)
{
    // --- Measure the content text (wrapped) with the window font -----------
    // GDI lays the text out for real: DT_CALCRECT wraps it at the max width
    // and returns the exact bounding size (CJK breaks per character).
    const int max_content_w = 360;   // long content wraps at this width
    HFONT font = generateFont();
    const scl2::Size text = measure_text(font, content, max_content_w);
    DeleteObject(font);

    // --- Client-area layout (logical px) -----------------------------------
    // Height = margins + label height + gap + button row; width =
    // max(label width, button row width) + margins.
    const int margin = 16, gap = 12;                 // gap: label -> buttons
    const int btn_w = 88, btn_h = 26, btn_gap = 8;
    const int btn_row_w = btn_w * 2 + btn_gap;
    const int client_w = std::max(text.w, btn_row_w) + margin * 2;
    const int client_h = margin + text.h + gap + btn_h + margin;

    // Geometry is the client-area size; the window adds the non-client area
    // (caption + borders) itself, so no manual AdjustWindowRectEx here.
    dialog<DialogButton> dlg(scl2::Rect{0, 0, client_w, client_h}, title);

    // Shared result written by the buttons (worker thread), read by the close
    // callback (also worker thread) and delivered through the future.
    auto result = std::make_shared<DialogButton>(DialogButton::Closed);

    // Content label: top-left, wrapped, sized to the measured text.
    auto content_lbl = new_label(scl2::Rect{margin, margin, text.w, text.h + 2}, content);
    content_lbl->set_wrap(true);
    dlg.add_child(content_lbl);

    // Button row at the bottom: OK right, Cancel to its left.
    const int by = margin + text.h + gap;
    const int ok_x = client_w - margin - btn_w;
    const int cancel_x = ok_x - btn_gap - btn_w;

    auto ok = new_button(scl2::Rect{ok_x, by, btn_w, btn_h}, L"OK");
    std::weak_ptr<button> ok_w = ok;
    ok->on_click([result, ok_w] {
        *result = DialogButton::Ok;
        if (auto p = ok_w.lock()) p->close_owner();
    });

    auto cancel = new_button(scl2::Rect{cancel_x, by, btn_w, btn_h}, L"Cancel");
    std::weak_ptr<button> cancel_w = cancel;
    cancel->on_click([result, cancel_w] {
        *result = DialogButton::Cancel;
        if (auto p = cancel_w.lock()) p->close_owner();
    });

    dlg.add_child(ok);
    dlg.add_child(cancel);

    dlg.bind_result([result](window&) { return *result; });
    dlg.show();
    return dlg;
}

// string (UTF-8) convenience overload: convert then delegate to the wide one.
dialog<DialogButton> askForConfirmation(const scl2::string& content, const scl2::string& title)
{
    return askForConfirmation(scl2::str_to_wstr(content), scl2::str_to_wstr(title));
}

window aboutSCF()
{
    ///TODO: ...
    return window();
}

control::control(scl2::wstring class_name, scl2::dword_t extra_style, scl2::Geometry geometry)
    : control(std::move(class_name), extra_style, 0, std::move(geometry))
{
}

control::control(scl2::wstring class_name, scl2::dword_t extra_style, scl2::Geometry geometry, scl2::wstring text)
    : control(std::move(class_name), extra_style, 0, std::move(geometry), std::move(text))
{
}

control::control(scl2::wstring class_name, scl2::dword_t extra_style, scl2::dword_t extra_ex_style, scl2::Geometry geometry)
    : m_class_name(std::move(class_name)), m_extra_style(extra_style), m_extra_ex_style(extra_ex_style), m_geometry(std::move(geometry))
{
}

control::control(scl2::wstring class_name, scl2::dword_t extra_style, scl2::dword_t extra_ex_style, scl2::Geometry geometry, scl2::wstring text)
    : m_class_name(std::move(class_name)), m_extra_style(extra_style), m_extra_ex_style(extra_ex_style), m_geometry(std::move(geometry)), m_text(std::move(text))
{
}

void control::create_control(control_id_t cid, scl2::winhandle_t parent_hwnd)
{
    // The control must have been attached to a window (window::add_child).
    if (!m_owner) {
        throw std::runtime_error("control: not attached to a window");
    }
    control_id = cid;

    // Suppress notifications sent while the control is created (e.g. an
    // initial EN_CHANGE for the preset text). The class-specific creation
    // style (m_extra_style, e.g. BS_AUTOCHECKBOX) is passed here once, in
    // dwStyle; later style edits (set_style) apply narrow masks on top of it.
    HWND hwnd;
    SYSCHANGE(hwnd = CreateWindowExW(
        m_extra_ex_style,
        m_class_name.c_str(),
        m_text.c_str(),
        WS_CHILD | WS_VISIBLE | m_extra_style,
        m_geometry.x, m_geometry.y, m_geometry.w, m_geometry.h,
        scl2::to_handle<HWND>(parent_hwnd),
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(control_id)),
        GetModuleHandleW(nullptr),
        nullptr
    ));

    if (!hwnd) {
        throw std::runtime_error("Failed to create control: " +
                                 platform::windows::TranslateLastError());
    }

    m_hwnd = scl2::from_handle(hwnd);

    // Apply style changes that were requested before the HWND existed
    // (set_style / set_alignment called before show()).
    if (m_pending_mask != 0) {
        HWND h = scl2::to_handle<HWND>(m_hwnd);
        DWORD cur = static_cast<DWORD>(GetWindowLongPtrW(h, GWL_STYLE));
        DWORD next = (cur & ~m_pending_mask) | (m_pending_style & m_pending_mask);
        SetWindowLongPtrW(h, GWL_STYLE, static_cast<LONG_PTR>(next));
        m_pending_style = 0;
        m_pending_mask = 0;
    }

    // Let subclasses apply pre-creation state (checkbox check, ...).
    on_created();
}

void control::set_style(scl2::dword_t style, scl2::dword_t mask)
{
    if (!m_hwnd) {
        // Not created yet (before show()): remember the change; it will be
        // composed and applied in create_control(). Multiple calls accumulate;
        // each only touches its own `mask` bits, so they never conflict with
        // each other or with the base creation style.
        m_pending_style = (m_pending_style & ~mask) | (style & mask);
        m_pending_mask |= mask;
        return;
    }
    HWND h = scl2::to_handle<HWND>(m_hwnd);
    DWORD cur = static_cast<DWORD>(GetWindowLongPtrW(h, GWL_STYLE));
    DWORD next = (cur & ~mask) | (style & mask);
    SetWindowLongPtrW(h, GWL_STYLE, static_cast<LONG_PTR>(next));
    InvalidateRect(h, nullptr, TRUE);
}

void control::set_text(const scl2::wstring& text)
{
    if (!m_hwnd) {
        // Not created yet: remember the text for creation time.
        m_text = text;
        return;
    }

    // Programmatic write: suppress the change notification this triggers so it
    // is not delivered as a user event (re-entrancy guard).
    SYSCHANGE(SetWindowTextW(scl2::to_handle<HWND>(m_hwnd), text.c_str()));
}

scl2::wstring control::get_text() const
{
    if (!m_hwnd) return m_text;

    HWND h = scl2::to_handle<HWND>(m_hwnd);
    const int len = GetWindowTextLengthW(h);
    scl2::wstring result(len, L'\0');
    if (len > 0) {
        GetWindowTextW(h, result.data(), len + 1);
    }
    return result;
}

void control::close_owner()
{
    if (m_owner) m_owner->close();
}

void control::set_event(event_id_t event_id, std::function<void()> fx)
{
    _event_callbacks[event_id] = fx;
}

void control::unset_event(event_id_t event_id)
{
    _event_callbacks.erase(event_id);
}

void control::handle_event(event_id_t notification)
{
    // Notifications caused by the program itself (creation / set_text) are
    // suppressed; only genuine user actions reach the callbacks.
    if (m_suppress_events.load()) return;

    auto it = _event_callbacks.find(notification);
    if (it != _event_callbacks.end()) {
        it->second();
    }
}


/*
    Control subclasses: button, label, etc. These are just thin wrappers around
    the base control class, with a fixed class name and some convenience methods.

    No, you cannot use control class directly. You are not meant to do that.
    These are the only things that you need to use.
*/


button::button(scl2::Geometry geometry, scl2::wstring text)
    : control(L"BUTTON", 0, geometry, std::move(text))
{}

void button::on_click(std::function<void()> fx)
{
    set_event(BN_CLICKED, fx);
}

label::label(scl2::Geometry geometry, scl2::wstring text)
    : control(L"STATIC", 0, geometry, std::move(text))
{
}

void label::set_alignment(scl2::Alignment a)
{
    // Map scl2::Alignment bit flags to Win32 SS_* static-text styles.
    scl2::dword_t ss = 0;
    if (a & scl2::Alignment::HCenter) ss |= SS_CENTER;
    else if (a & scl2::Alignment::Right) ss |= SS_RIGHT;
    // else: left (SS_LEFT == 0, the default)

    if (a & scl2::Alignment::VCenter) ss |= 0x0200;      // SS_VCENTER
    else if (a & scl2::Alignment::Bottom) ss |= 0x0100;  // SS_BOTTOM
    // else: top (SS_TOP == 0, the default)

    // Only touch the alignment bits: the horizontal type bits (SS_TYPEMASK)
    // plus the vertical SS_VCENTER / SS_BOTTOM bits (outside the mask).
    set_style(ss, SS_TYPEMASK | 0x0200 | 0x0100);
}

void label::set_wrap(bool wrap)
{
    // SS_EDITCONTROL makes a STATIC wrap at word boundaries / CJK characters
    // (edit-control line breaking) instead of only at spaces.
    set_style(wrap ? SS_EDITCONTROL : 0, SS_EDITCONTROL);
}

checkbox::checkbox(scl2::Geometry geometry, scl2::wstring text)
    : control(L"BUTTON", BS_AUTOCHECKBOX, geometry, std::move(text))
{}

bool checkbox::is_checked() const
{
    if (!m_hwnd) return m_checked;
    return SendMessageW(scl2::to_handle<HWND>(m_hwnd), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void checkbox::set_checked(bool checked)
{
    if (!m_hwnd) { m_checked = checked; return; }
    SendMessageW(scl2::to_handle<HWND>(m_hwnd), BM_SETCHECK,
                 checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

void checkbox::on_click(std::function<void()> fx)
{
    set_event(BN_CLICKED, fx);
}

void checkbox::on_created()
{
    // Apply the checked state captured before the HWND existed.
    if (m_hwnd) {
        SendMessageW(scl2::to_handle<HWND>(m_hwnd), BM_SETCHECK,
                     m_checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

textbox::textbox(scl2::Geometry geometry, scl2::wstring text)
    : control(L"EDIT", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, geometry, std::move(text))
{}

void textbox::on_change(std::function<void()> fx)
{
    set_event(EN_CHANGE, fx);
}

void textbox::on_update(std::function<void()> fx)
{
    set_event(EN_UPDATE, fx);
}

void textbox::on_setfocus(std::function<void()> fx)
{
    set_event(EN_SETFOCUS, fx);
}

void textbox::on_killfocus(std::function<void()> fx)
{
    set_event(EN_KILLFOCUS, fx);
}

void textbox::on_enter(std::function<void()> fx)
{
    set_event(SCF_EN_ENTER, std::move(fx));
    m_want_enter = true;   // install the Enter-key subclass at creation
}

void textbox::set_read_only(bool ro)
{
    if (!m_hwnd) { m_read_only = ro; return; }
    SendMessageW(scl2::to_handle<HWND>(m_hwnd), EM_SETREADONLY, ro ? TRUE : FALSE, 0);
}

void textbox::set_password(bool pw)
{
    set_style(pw ? ES_PASSWORD : 0, ES_PASSWORD);
}

void textbox::on_created()
{
    if (!m_hwnd) return;
    HWND h = scl2::to_handle<HWND>(m_hwnd);
    // Apply state captured before the HWND existed.
    if (m_read_only) SendMessageW(h, EM_SETREADONLY, TRUE, 0);
    if (m_want_enter) SetWindowSubclass(h, scfEditSubclass, 0, 0);
}

groupbox::groupbox(scl2::Geometry geometry, scl2::wstring text)
    : control(L"BUTTON", BS_GROUPBOX, geometry, std::move(text))
{}

radio::radio(scl2::Geometry geometry, scl2::wstring text)
    : control(L"BUTTON", BS_AUTORADIOBUTTON, geometry, std::move(text))
{}

bool radio::is_checked() const
{
    if (!m_hwnd) return m_checked;
    return SendMessageW(scl2::to_handle<HWND>(m_hwnd), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void radio::set_checked(bool checked)
{
    if (!m_hwnd) { m_checked = checked; return; }
    SendMessageW(scl2::to_handle<HWND>(m_hwnd), BM_SETCHECK,
                 checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

void radio::on_click(std::function<void()> fx)
{
    set_event(BN_CLICKED, std::move(fx));
}

void radio::on_created()
{
    // Apply the checked state captured before the HWND existed.
    if (m_hwnd) {
        SendMessageW(scl2::to_handle<HWND>(m_hwnd), BM_SETCHECK,
                     m_checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

radiogroup::radiogroup(scl2::Geometry geometry, scl2::wstring title)
    : control(L"BUTTON", BS_GROUPBOX, geometry, std::move(title))
{}

std::shared_ptr<radio> radiogroup::add(const scl2::wstring& text)
{
    // Stack the options vertically inside the frame (logical px).
    const int radio_h = 22;
    const int left = 14;                 // clear the frame line
    const int top = 16 + static_cast<int>(m_radios.size()) * radio_h;
    const int w = m_geometry.w - left - 8;
    auto r = new_radio(scl2::Rect{m_geometry.x + left, m_geometry.y + top, w, radio_h}, text);

    if (m_radios.empty()) {
        // The first radio leads the group: WS_GROUP + WS_TABSTOP make Windows
        // keep the radios mutually exclusive and arrow-key navigable.
        // (radiogroup is a friend of control, so it may set the style here.)
        r->set_style(WS_GROUP | WS_TABSTOP, WS_GROUP | WS_TABSTOP);
    }

    // Forward the click to the group's on_select callback.
    std::weak_ptr<radio> wr = r;
    r->on_click([this, wr] {
        auto p = wr.lock();
        if (!p) return;
        for (size_t i = 0; i < m_radios.size(); ++i) {
            if (m_radios[i] == p) {
                if (m_on_select) m_on_select(static_cast<int>(i));
                return;
            }
        }
    });

    m_radios.push_back(std::move(r));
    return m_radios.back();
}

int radiogroup::selected_index() const
{
    for (size_t i = 0; i < m_radios.size(); ++i) {
        if (m_radios[i]->is_checked()) return static_cast<int>(i);
    }
    return -1;
}

void radiogroup::select(int index)
{
    for (size_t i = 0; i < m_radios.size(); ++i) {
        m_radios[i]->set_checked(static_cast<int>(i) == index);
    }
}

void radiogroup::on_select(std::function<void(int)> fx)
{
    m_on_select = std::move(fx);
}

void radiogroup::collect_sub_controls(std::vector<std::shared_ptr<control>>& out) const
{
    for (auto& r : m_radios) out.push_back(r);
}

} // namespace scf