/*  scf_tray.hpp -- system tray (notification area) support for scf.  */
/*
    Simple Framework Cpp
    Tianming Wu <github.com/Tianming-Wu>

    Low-level tray-icon building blocks: an RAII HICON wrapper, an RAII menu,
    and a tray_icon that attaches to a window. Loading/decoding image files
    into an HICON is intentionally left out -- that belongs to a higher layer
    (e.g. a QIcon-like Framework); here you hand over an existing HICON.
*/

#pragma once

#include "scf.hpp"

#include <windows.h>
#include <shellapi.h>

#include <functional>
#include <string>

namespace scf {

// RAII owner of a HICON, mirroring how EnumWindow-T's Framework::Font owns an
// HFONT. The constructor / reset() adopt the handle and destroy it on
// destruction or replacement.
class SCF_EXPORT icon {
public:
    icon() = default;
    explicit icon(HICON hicon) : m_hicon(hicon) {}
    ~icon() { reset(); }

    icon(const icon&) = delete;
    icon& operator=(const icon&) = delete;

    icon(icon&& other) noexcept : m_hicon(other.m_hicon) { other.m_hicon = nullptr; }
    icon& operator=(icon&& other) noexcept {
        if (this != &other) {
            reset();
            m_hicon = other.m_hicon;
            other.m_hicon = nullptr;
        }
        return *this;
    }

    HICON handle() const { return m_hicon; }
    explicit operator bool() const { return m_hicon != nullptr; }

    // Take ownership of a new handle (releases the current one).
    void reset(HICON hicon = nullptr) {
        if (m_hicon) DestroyIcon(m_hicon);
        m_hicon = hicon;
    }

private:
    HICON m_hicon = nullptr;
};

// RAII owner of a Win32 popup menu (HMENU).
class SCF_EXPORT menu {
public:
    menu();
    ~menu();
    menu(const menu&) = delete;
    menu& operator=(const menu&) = delete;
    menu(menu&& other) noexcept;
    menu& operator=(menu&& other) noexcept;

    HMENU handle() const { return m_menu; }
    explicit operator bool() const { return m_menu != nullptr; }

    menu& item(const std::wstring& text, unsigned int id);
    menu& separator();

private:
    HMENU m_menu = nullptr;
};

// A system tray icon attached to a window. Construct it after the window is
// shown, then call show() (Shell_NotifyIcon NIM_ADD). The tray_icon holds the
// window's shared implementation, so it survives the window facade being
// moved; it must simply outlive the underlying window itself.
class SCF_EXPORT tray_icon {
public:
    tray_icon(window& owner, const icon& ic, const std::wstring& tooltip = L"");
    ~tray_icon();   // removes the icon if shown and unregisters the hook

    tray_icon(const tray_icon&) = delete;
    tray_icon& operator=(const tray_icon&) = delete;

    // NIM_ADD. Requires the owner window's HWND (i.e. it must have been
    // shown). Returns false if the window is not up or the shell rejected it.
    bool show();
    void hide();    // NIM_DELETE; idempotent
    bool shown() const { return m_shown; }

    void set_icon(const icon& ic);     // NIM_MODIFY
    void set_tooltip(const std::wstring& tip);
    void show_balloon(const std::wstring& title, const std::wstring& text,
                      unsigned int flags = 0);   // flags: 0=none 1=info 2=warning 4=error

    // Events, delivered on the window's worker thread.
    void on_click(std::function<void()> cb);          // left click
    void on_double_click(std::function<void()> cb);   // left double click
    void on_right_click(std::function<void()> cb);    // right click (usually shows a menu)
    void on_balloon_click(std::function<void()> cb);

    // Show a context menu at the cursor; returns the chosen item id, or 0 if
    // the user dismissed it. Call it from an on_right_click handler.
    unsigned int popup_menu(const menu& m) const;

private:
    bool on_message(std::uintptr_t wParam, std::intptr_t lParam);
    void rebuild_and_notify(UINT action, bool include_balloon = false);

    std::shared_ptr<window::impl> m_impl;   // stable across window moves
    HICON m_hicon = nullptr;   // owned copy (CopyIcon)
    std::wstring m_tooltip;
    std::wstring m_balloon_title, m_balloon_text;
    unsigned int m_balloon_flags = 0;
    unsigned int m_id = 0;         // unique per-process icon id (uID)
    unsigned int m_message = 0;    // callback message (uCallbackMessage)
    bool m_shown = false;

    std::function<void()> m_on_click, m_on_dbl, m_on_rclick, m_on_balloon_click;
};

} // namespace scf
