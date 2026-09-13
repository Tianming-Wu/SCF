/*  scf_listbox.cpp -- listbox (LISTBOX) implementation.

    A header-less single-column list of strings. Unlike listview this control
    reports through WM_COMMAND (LBN_SELCHANGE / LBN_DBLCLK), so it rides the
    existing dispatch_command() path and needs no notify plumbing.

    The class owns its model and replays it at creation, so entries can be added
    before show(). With LBS_SORT the CONTROL picks the ordering, so add() takes
    the position the control reports back and mirrors it in the model -- that
    keeps an index meaning the same thing to both sides.                        */

#include "scf.hpp"
#include "scf_internal.hpp"

#include <SharedCppLib2/platform.hpp>

#include <windows.h>

namespace scf {

listbox::listbox(scl2::Geometry geometry)
    : control(L"LISTBOX",
              WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
              WS_EX_CLIENTEDGE,
              geometry, L"")
{
}

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

void listbox::bind_events()
{
    // LISTBOX notifications arrive as WM_COMMAND, so they go through the same
    // _event_callbacks map a button uses -- the index is read back at the time
    // the callback runs.
    set_event(LBN_SELCHANGE, [this] {
        if (m_on_selection_changed) m_on_selection_changed(selected_index());
    });
    set_event(LBN_DBLCLK, [this] {
        if (m_on_activate) m_on_activate(selected_index());
    });
}

void listbox::on_created()
{
    if (!m_hwnd) return;

    bind_events();

    // Replay what was queued before show(). Hand the model to a local first so
    // add() can rebuild it in the control's order (LBS_SORT may reorder).
    std::vector<item> replay;
    replay.swap(m_items);
    for (const item& it : replay) add(it.text, it.user_data);

    if (m_selected >= 0) select(m_selected);
}

// ---------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------

int listbox::add(const scl2::wstring& text, std::uintptr_t user_data)
{
    if (m_hwnd) {
        HWND h = scl2::to_handle<HWND>(m_hwnd);
        const int at = static_cast<int>(
            SendMessageW(h, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())));
        if (at < 0) return -1;   // LB_ERR / LB_ERRSPACE

        SendMessageW(h, LB_SETITEMDATA, static_cast<WPARAM>(at),
                     static_cast<LPARAM>(user_data));

        item it;
        it.text = text;
        it.user_data = user_data;
        // Insert where the control put it, not necessarily at the end.
        if (at <= static_cast<int>(m_items.size())) {
            m_items.insert(m_items.begin() + at, std::move(it));
        } else {
            m_items.push_back(std::move(it));
        }
        return at;
    }

    item it;
    it.text = text;
    it.user_data = user_data;
    m_items.push_back(std::move(it));
    return static_cast<int>(m_items.size()) - 1;
}

void listbox::remove(int index)
{
    if (index < 0 || index >= count()) return;

    m_items.erase(m_items.begin() + index);
    if (m_selected == index) m_selected = -1;
    else if (m_selected > index) --m_selected;

    if (!m_hwnd) return;
    SendMessageW(scl2::to_handle<HWND>(m_hwnd), LB_DELETESTRING,
                 static_cast<WPARAM>(index), 0);
}

void listbox::set_item_text(int index, const scl2::wstring& text)
{
    if (index < 0 || index >= count()) return;

    item& it = m_items[static_cast<size_t>(index)];
    if (!m_hwnd) { it.text = text; return; }

    HWND h = scl2::to_handle<HWND>(m_hwnd);

    // LISTBOX has no LB_SETSTRING, so replace the entry where it stands: delete
    // it and re-insert at the same index, then restore its item data. (With
    // LBS_SORT enabled the control may pick a different position instead, in
    // which case only the text is guaranteed to be right.)
    sys_change([&] {
        SendMessageW(h, LB_DELETESTRING, static_cast<WPARAM>(index), 0);
        const int at = static_cast<int>(
            SendMessageW(h, LB_INSERTSTRING, static_cast<WPARAM>(index),
                         reinterpret_cast<LPARAM>(text.c_str())));
        if (at >= 0) {
            SendMessageW(h, LB_SETITEMDATA, static_cast<WPARAM>(at),
                         static_cast<LPARAM>(it.user_data));
        }
    });

    it.text = text;
}

scl2::wstring listbox::item_text(int index) const
{
    if (index < 0 || index >= count()) return L"";
    return m_items[static_cast<size_t>(index)].text;
}

void listbox::set_item_data(int index, std::uintptr_t user_data)
{
    if (index < 0 || index >= count()) return;
    m_items[static_cast<size_t>(index)].user_data = user_data;

    if (!m_hwnd) return;
    SendMessageW(scl2::to_handle<HWND>(m_hwnd), LB_SETITEMDATA,
                 static_cast<WPARAM>(index), static_cast<LPARAM>(user_data));
}

std::uintptr_t listbox::item_data(int index) const
{
    if (index < 0 || index >= count()) return 0;
    return m_items[static_cast<size_t>(index)].user_data;
}

void listbox::clear()
{
    m_items.clear();
    m_selected = -1;

    if (!m_hwnd) return;
    SendMessageW(scl2::to_handle<HWND>(m_hwnd), LB_RESETCONTENT, 0, 0);
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

int listbox::selected_index() const
{
    if (!m_hwnd) return m_selected;
    return static_cast<int>(
        SendMessageW(scl2::to_handle<HWND>(m_hwnd), LB_GETCURSEL, 0, 0));
}

std::vector<int> listbox::selected_indices() const
{
    std::vector<int> out;

    if (!m_hwnd) {
        if (m_selected >= 0) out.push_back(m_selected);
        return out;
    }

    HWND h = scl2::to_handle<HWND>(m_hwnd);
    const int n = static_cast<int>(SendMessageW(h, LB_GETSELCOUNT, 0, 0));
    if (n <= 0) return out;

    out.resize(static_cast<size_t>(n));
    SendMessageW(h, LB_GETSELITEMS, static_cast<WPARAM>(n),
                 reinterpret_cast<LPARAM>(out.data()));
    return out;
}

void listbox::select(int index)
{
    m_selected = index;
    if (!m_hwnd) return;

    HWND h = scl2::to_handle<HWND>(m_hwnd);

    // LB_SETCURSEL does not raise LBN_SELCHANGE, but wrap it anyway so the
    // "programmatic changes stay quiet" rule matches listview exactly.
    sys_change([&] {
        SendMessageW(h, LB_SETCURSEL,
                     static_cast<WPARAM>(index < 0 ? -1 : index), 0);
    });
}

void listbox::set_multi_select(bool on)
{
    // LBS_EXTENDEDSEL is a creation-time style: the control decides how it
    // tracks selection when it is created, so queue it on the creation style
    // rather than poking GWL_STYLE. Must precede show().
    set_creation_style(on ? LBS_EXTENDEDSEL : 0u, LBS_EXTENDEDSEL);
}

void listbox::ensure_visible(int index)
{
    if (!m_hwnd || index < 0 || index >= count()) return;

    // There is no "make this entry visible" message: LB_SETTOPINDEX always
    // scrolls the entry to the top, so only use it when the entry is actually
    // outside the visible range.
    HWND h = scl2::to_handle<HWND>(m_hwnd);
    const int top = static_cast<int>(SendMessageW(h, LB_GETTOPINDEX, 0, 0));
    const int item_h = static_cast<int>(SendMessageW(h, LB_GETITEMHEIGHT, 0, 0));

    RECT rc{};
    GetClientRect(h, &rc);
    const int visible = (item_h > 0) ? (rc.bottom - rc.top) / item_h : 1;
    if (visible <= 0) return;

    if (index < top) {
        SendMessageW(h, LB_SETTOPINDEX, static_cast<WPARAM>(index), 0);
    } else if (index >= top + visible) {
        const int new_top = index - visible + 1;
        SendMessageW(h, LB_SETTOPINDEX, static_cast<WPARAM>(new_top), 0);
    }
}

void listbox::set_sort(bool on)
{
    // LBS_SORT is read when the control is created; setting the bit afterwards
    // does NOT make an existing LISTBOX sort what it already holds. It has to
    // be in the creation style, so set_creation_style() -- and therefore this
    // call -- must precede show().
    set_creation_style(on ? LBS_SORT : 0u, LBS_SORT);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void listbox::on_selection_changed(std::function<void(int index)> fx)
{
    m_on_selection_changed = std::move(fx);
}

void listbox::on_activate(std::function<void(int index)> fx)
{
    m_on_activate = std::move(fx);
}

// ---------------------------------------------------------------------------
// Appearance
// ---------------------------------------------------------------------------

void listbox::set_batch_mode(bool on)
{
    if (!m_hwnd) return;

    HWND h = scl2::to_handle<HWND>(m_hwnd);
    SendMessageW(h, WM_SETREDRAW, on ? FALSE : TRUE, 0);
    if (!on) InvalidateRect(h, nullptr, TRUE);
}

} // namespace scf
