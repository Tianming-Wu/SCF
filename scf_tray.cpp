// scf_tray.cpp -- system tray (notification area) support for scf.

#include "scf_tray.hpp"
#include "scf_internal.hpp"   // window::impl (tray_icon reaches it directly)

#include <cstring>

namespace scf {

namespace {
// Unique per-process identity for each tray icon: uID and the callback
// message must be unique per (window, icon) so NIM_MODIFY / NIM_DELETE and
// the message dispatch are unambiguous.
std::atomic<unsigned int> g_tray_counter{0};
constexpr unsigned int kTrayMessageBase = 0x8100;   // WM_APP area (0x8000+)
// NIN_BALLOONUSERCLICK == WM_USER + 5, written literally to avoid needing a
// high _WIN32_IE in this build.
constexpr long kBalloonClick = 0x0405;
} // namespace

menu::menu()
    : m_menu(CreatePopupMenu())
{
}

menu::~menu()
{
    if (m_menu) DestroyMenu(m_menu);
}

menu::menu(menu&& other) noexcept : m_menu(other.m_menu) { other.m_menu = nullptr; }
menu& menu::operator=(menu&& other) noexcept
{
    if (this != &other) {
        if (m_menu) DestroyMenu(m_menu);
        m_menu = other.m_menu;
        other.m_menu = nullptr;
    }
    return *this;
}

menu& menu::item(const std::wstring& text, unsigned int id)
{
    if (m_menu) AppendMenuW(m_menu, MF_STRING, id, text.c_str());
    return *this;
}

menu& menu::separator()
{
    if (m_menu) AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    return *this;
}

tray_icon::tray_icon(window& owner, const icon& ic, const std::wstring& tooltip)
    : m_impl(owner.shared_impl()), m_tooltip(tooltip)
{
    const unsigned int n = g_tray_counter.fetch_add(1);
    m_id = 1 + n;
    m_message = kTrayMessageBase + n;
    m_hicon = ic.handle() ? CopyIcon(ic.handle()) : nullptr;

    // Route our callback message to on_message() on the window's thread. The
    // hook lives in the shared impl, so it survives facade moves.
    if (m_impl) {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        m_impl->message_hooks[m_message] = [this](std::uintptr_t w, std::intptr_t l) {
            return on_message(w, l);
        };
    }
}

tray_icon::~tray_icon()
{
    hide();   // NIM_DELETE if shown
    if (m_impl) {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        m_impl->message_hooks.erase(m_message);
    }
    if (m_hicon) DestroyIcon(m_hicon);
}

bool tray_icon::show()
{
    if (m_shown) return true;
    HWND hwnd = m_impl ? m_impl->hwnd.load() : nullptr;
    if (!hwnd) return false;
    rebuild_and_notify(NIM_ADD);
    m_shown = true;

    // Ask for modern tray behavior (mouse-move + NIN_* notifications).
    // With NOTIFYICON_VERSION_4 the right-click arrives as WM_CONTEXTMENU.
    // NOTIFYICON_VERSION_4 == 4.
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = m_id;
    nid.uVersion = 4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    return true;
}

void tray_icon::hide()
{
    if (!m_shown) return;
    HWND hwnd = m_impl ? m_impl->hwnd.load() : nullptr;
    if (hwnd) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = hwnd;
        nid.uID = m_id;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    m_shown = false;
}

void tray_icon::set_icon(const icon& ic)
{
    HICON h = ic.handle() ? CopyIcon(ic.handle()) : nullptr;
    if (m_hicon) DestroyIcon(m_hicon);
    m_hicon = h;
    if (m_shown) rebuild_and_notify(NIM_MODIFY);
}

void tray_icon::set_tooltip(const std::wstring& tip)
{
    m_tooltip = tip;
    if (m_shown) rebuild_and_notify(NIM_MODIFY);
}

void tray_icon::show_balloon(const std::wstring& title, const std::wstring& text,
                             unsigned int flags)
{
    m_balloon_title = title;
    m_balloon_text = text;
    m_balloon_flags = flags;
    if (m_shown) rebuild_and_notify(NIM_MODIFY, true);
}

void tray_icon::on_click(std::function<void()> cb) { m_on_click = std::move(cb); }
void tray_icon::on_double_click(std::function<void()> cb) { m_on_dbl = std::move(cb); }
void tray_icon::on_right_click(std::function<void()> cb) { m_on_rclick = std::move(cb); }
void tray_icon::on_balloon_click(std::function<void()> cb) { m_on_balloon_click = std::move(cb); }

void tray_icon::rebuild_and_notify(UINT action, bool include_balloon)
{
    HWND hwnd = m_impl ? m_impl->hwnd.load() : nullptr;
    if (!hwnd) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = m_id;
    nid.uFlags = NIF_MESSAGE;
    nid.uCallbackMessage = m_message;
    if (m_hicon) {
        nid.uFlags |= NIF_ICON;
        nid.hIcon = m_hicon;
    }
    if (!m_tooltip.empty()) {
        nid.uFlags |= NIF_TIP;
        wcscpy_s(nid.szTip, m_tooltip.c_str());
    }
    if (include_balloon) {
        nid.uFlags |= NIF_INFO;
        nid.dwInfoFlags = m_balloon_flags;
        wcscpy_s(nid.szInfoTitle, m_balloon_title.c_str());
        wcscpy_s(nid.szInfo, m_balloon_text.c_str());
    }
    Shell_NotifyIconW(action, &nid);
}

bool tray_icon::on_message(std::uintptr_t wParam, std::intptr_t lParam)
{
    // The tray callback arrives on this icon's private message. The low word
    // of lParam holds the mouse message / notification; the shell ORs in a
    // 0x10000 flag, and wParam is not reliably the icon id -- so mask the low
    // word and ignore wParam (each tray_icon has its own private message, so
    // there is no ambiguity between icons).
    (void)wParam;
    const LONG msg = static_cast<LONG>(lParam) & 0xFFFF;
    switch (msg) {
        case WM_LBUTTONUP:
            if (m_on_click) m_on_click();
            return true;
        case WM_LBUTTONDBLCLK:
            if (m_on_dbl) m_on_dbl();
            return true;
        case WM_CONTEXTMENU:     // right-click with NOTIFYICON_VERSION_4
        case WM_RBUTTONUP:       // legacy right-click (no NIM_SETVERSION)
            if (m_on_rclick) m_on_rclick();
            return true;
        case kBalloonClick:
            if (m_on_balloon_click) m_on_balloon_click();
            return true;
        default:
            return true;   // our custom message: consume it
    }
}

unsigned int tray_icon::popup_menu(const menu& m) const
{
    HWND hwnd = m_impl ? m_impl->hwnd.load() : nullptr;
    if (!hwnd || !m.handle()) return 0;
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);   // so the menu dismisses on an outside click
    const UINT cmd = TrackPopupMenu(m.handle(),
                                    TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD,
                                    pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);   // keep the menu from lingering
    return cmd;
}

} // namespace scf
