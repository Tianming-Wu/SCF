/*  scf_listview.cpp -- listview (SysListView32) implementation.

    Kept in its own translation unit because it is the only control so far
    that needs <commctrl.h>; scf.hpp itself must stay windows.h-free, so the
    class is declared there and implemented here. The public surface is
    described in scf.hpp.

    The class is the single source of truth for its model (columns + rows) and
    replays that model into the OS control when it is created, which is what
    lets callers fill a list before show().                                   */

#include "scf.hpp"
#include "scf_internal.hpp"

#include <SharedCppLib2/platform.hpp>

#include <algorithm>

#include <windows.h>
#include <commctrl.h>

namespace scf {

listview::listview(scl2::Geometry geometry)
    : control(L"SysListView32",
              WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
              WS_EX_CLIENTEDGE,
              geometry, L"")
{
}

// ---------------------------------------------------------------------------
// Creation / DPI
// ---------------------------------------------------------------------------

void listview::on_created()
{
    if (!m_hwnd) return;
    HWND h = scl2::to_handle<HWND>(m_hwnd);
    const int dpi = static_cast<int>(GetDpiForWindow(h));

    apply_extended_styles();
    insert_columns(dpi);

    // Replay everything queued before show().
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) insert_row(i);

    if (m_selected >= 0) select_row(m_selected);
}

void listview::on_dpi_changed(int dpi)
{
    // Auto-width columns are sized from the control's client area, so they need
    // re-fitting whenever the control's physical size changes. Controls here are
    // absolutely positioned, so that only ever happens on a DPI change (and once
    // at creation) -- which is exactly when this is called.
    if (!m_hwnd) return;
    set_column_widths(dpi);
}

void listview::apply_extended_styles()
{
    if (!m_hwnd) return;
    HWND h = scl2::to_handle<HWND>(m_hwnd);

    DWORD ex = LVS_EX_DOUBLEBUFFER;          // no flicker while scrolling
    if (m_full_row_select) ex |= LVS_EX_FULLROWSELECT;
    if (m_grid_lines) ex |= LVS_EX_GRIDLINES;

    // wParam = mask of the bits to change, lParam = their new values.
    SendMessageW(h, LVM_SETEXTENDEDLISTVIEWSTYLE,
                 static_cast<WPARAM>(ex), static_cast<LPARAM>(ex));
}

void listview::insert_columns(int dpi)
{
    for (int i = 0; i < static_cast<int>(m_columns.size()); ++i) insert_column_at(i, dpi);
    // Auto-width columns can only be resolved once every column exists.
    set_column_widths(dpi);
}

void listview::insert_column_at(int index, int dpi)
{
    HWND h = scl2::to_handle<HWND>(m_hwnd);
    const column_def& col = m_columns[static_cast<size_t>(index)];

    LVCOLUMNW lvc{};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
    lvc.pszText = const_cast<wchar_t*>(col.title.c_str());
    lvc.cx = ::MulDiv(col.width, dpi, 96);   // logical -> physical
    lvc.fmt = static_cast<int>(col.alignment);
    lvc.iSubItem = index;

    SendMessageW(h, LVM_INSERTCOLUMNW,
                 static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&lvc));
}

void listview::set_column_widths(int dpi)
{
    HWND h = scl2::to_handle<HWND>(m_hwnd);

    RECT rc{};
    GetClientRect(h, &rc);
    const int client_w = rc.right - rc.left;

    // Fixed-width columns claim their share first; the auto ones (width <= 0)
    // divide whatever is left. Without a fixed column this is simply the full
    // client width, which is what a header-less single-column list wants.
    int fixed = 0;
    for (const column_def& c : m_columns) {
        if (c.width > 0) fixed += ::MulDiv(c.width, dpi, 96);
    }
    const int auto_w = (std::max)(16, client_w - fixed);

    for (int i = 0; i < static_cast<int>(m_columns.size()); ++i) {
        const int px = (m_columns[static_cast<size_t>(i)].width > 0)
                           ? ::MulDiv(m_columns[static_cast<size_t>(i)].width, dpi, 96)
                           : auto_w;
        SendMessageW(h, LVM_SETCOLUMNWIDTH,
                     static_cast<WPARAM>(i), static_cast<LPARAM>(px));
    }
}

void listview::insert_row(int index)
{
    HWND h = scl2::to_handle<HWND>(m_hwnd);
    const row_def& row = m_rows[static_cast<size_t>(index)];

    LVITEMW lvi{};
    lvi.mask = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem = index;
    lvi.pszText = const_cast<wchar_t*>(row.cells.empty() ? L"" : row.cells[0].c_str());
    lvi.lParam = static_cast<LPARAM>(row.user_data);

    const int at = static_cast<int>(
        SendMessageW(h, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&lvi)));
    if (at < 0) return;   // the control refused the item

    // Remaining cells go in as sub-items of the row we just added.
    for (int c = 1; c < static_cast<int>(row.cells.size()); ++c) {
        LVITEMW sub{};
        sub.mask = LVIF_TEXT;
        sub.iItem = at;
        sub.iSubItem = c;
        sub.pszText = const_cast<wchar_t*>(row.cells[static_cast<size_t>(c)].c_str());
        SendMessageW(h, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&sub));
    }
}

// ---------------------------------------------------------------------------
// Columns
// ---------------------------------------------------------------------------

void listview::add_column(const scl2::wstring& title, int width, column_alignment alignment)
{
    column_def def;
    def.title = title;
    def.width = width;   // <= 0 means "take the remaining width"
    def.alignment = alignment;
    m_columns.push_back(std::move(def));

    if (!m_hwnd) return;   // replayed by insert_columns() at creation
    insert_column_at(static_cast<int>(m_columns.size()) - 1,
                     static_cast<int>(GetDpiForWindow(scl2::to_handle<HWND>(m_hwnd))));
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

int listview::add_row(const scl2::wstring& text, std::uintptr_t user_data)
{
    row_def row;
    row.cells.push_back(text);
    row.user_data = user_data;

    const int index = static_cast<int>(m_rows.size());
    m_rows.push_back(std::move(row));

    if (m_hwnd) insert_row(index);
    return index;
}

void listview::set_cell(int row, int column, const scl2::wstring& text)
{
    if (row < 0 || row >= row_count() || column < 0) return;

    row_def& r = m_rows[static_cast<size_t>(row)];
    if (static_cast<size_t>(column) >= r.cells.size()) {
        r.cells.resize(static_cast<size_t>(column) + 1);
    }
    r.cells[static_cast<size_t>(column)] = text;

    if (!m_hwnd) return;
    HWND h = scl2::to_handle<HWND>(m_hwnd);

    LVITEMW lvi{};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = row;
    lvi.iSubItem = column;
    lvi.pszText = const_cast<wchar_t*>(r.cells[static_cast<size_t>(column)].c_str());
    SendMessageW(h, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&lvi));
}

scl2::wstring listview::get_cell(int row, int column) const
{
    if (row < 0 || row >= row_count() || column < 0) return L"";
    const row_def& r = m_rows[static_cast<size_t>(row)];
    if (static_cast<size_t>(column) >= r.cells.size()) return L"";
    return r.cells[static_cast<size_t>(column)];
}

void listview::set_row_data(int row, std::uintptr_t user_data)
{
    if (row < 0 || row >= row_count()) return;
    m_rows[static_cast<size_t>(row)].user_data = user_data;

    if (!m_hwnd) return;
    HWND h = scl2::to_handle<HWND>(m_hwnd);

    LVITEMW lvi{};
    lvi.mask = LVIF_PARAM;
    lvi.iItem = row;
    lvi.lParam = static_cast<LPARAM>(user_data);
    SendMessageW(h, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&lvi));
}

std::uintptr_t listview::row_data(int row) const
{
    if (row < 0 || row >= row_count()) return 0;
    return m_rows[static_cast<size_t>(row)].user_data;
}

void listview::remove_row(int row)
{
    if (row < 0 || row >= row_count()) return;

    m_rows.erase(m_rows.begin() + row);
    if (m_selected == row) m_selected = -1;
    else if (m_selected > row) --m_selected;

    if (!m_hwnd) return;
    SendMessageW(scl2::to_handle<HWND>(m_hwnd), LVM_DELETEITEM,
                 static_cast<WPARAM>(row), 0);
}

void listview::clear()
{
    m_rows.clear();
    m_selected = -1;

    if (!m_hwnd) return;
    SendMessageW(scl2::to_handle<HWND>(m_hwnd), LVM_DELETEALLITEMS, 0, 0);
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void listview::set_multi_select(bool on)
{
    m_multi_select = on;
    // LVS_SINGLESEL forces single selection; clearing the bit restores the
    // default Ctrl/Shift multi-selection. set_style() also handles the
    // pre-show case (queued until creation).
    set_style(on ? 0u : LVS_SINGLESEL, LVS_SINGLESEL);
}

int listview::selected_row() const
{
    if (!m_hwnd) return m_selected;
    HWND h = scl2::to_handle<HWND>(m_hwnd);
    return static_cast<int>(SendMessageW(h, LVM_GETNEXTITEM,
                                         static_cast<WPARAM>(-1), LVNI_SELECTED));
}

std::vector<int> listview::selected_rows() const
{
    std::vector<int> out;

    if (!m_hwnd) {
        if (m_selected >= 0) out.push_back(m_selected);
        return out;
    }

    HWND h = scl2::to_handle<HWND>(m_hwnd);
    int i = -1;
    while (true) {
        i = static_cast<int>(SendMessageW(h, LVM_GETNEXTITEM,
                                          static_cast<WPARAM>(i), LVNI_SELECTED));
        if (i < 0) break;
        out.push_back(i);
    }
    return out;
}

void listview::select_row(int row)
{
    m_selected = row;
    if (!m_hwnd) return;

    HWND h = scl2::to_handle<HWND>(m_hwnd);

    // Choosing a row programmatically is not a user action: wrap the writes in
    // sys_change() so the LVN_ITEMCHANGED they raise is not reported back
    // through on_selection_changed().
    sys_change([&] {
        if (row < 0) {
            // Clear every selected item.
            int i = -1;
            while ((i = static_cast<int>(SendMessageW(h, LVM_GETNEXTITEM,
                                                      static_cast<WPARAM>(i), LVNI_SELECTED))) >= 0) {
                LVITEMW lvi{};
                lvi.mask = LVIF_STATE;
                lvi.iItem = i;
                lvi.stateMask = LVIS_SELECTED;
                lvi.state = 0;
                SendMessageW(h, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&lvi));
            }
            return;
        }

        LVITEMW lvi{};
        lvi.mask = LVIF_STATE;
        lvi.iItem = row;
        lvi.stateMask = LVIS_SELECTED;
        lvi.state = LVIS_SELECTED;
        SendMessageW(h, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&lvi));
    });
}

void listview::ensure_visible(int row)
{
    if (!m_hwnd || row < 0 || row >= row_count()) return;
    SendMessageW(scl2::to_handle<HWND>(m_hwnd), LVM_ENSUREVISIBLE,
                 static_cast<WPARAM>(row), FALSE);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void listview::on_selection_changed(std::function<void(int row)> fx)
{
    m_on_selection_changed = std::move(fx);
}

void listview::on_activate(std::function<void(int row)> fx)
{
    m_on_activate = std::move(fx);
}

void listview::on_click(std::function<void(int row)> fx)
{
    m_on_click = std::move(fx);
}

void listview::handle_notify(const notify_info& info)
{
    switch (info.code) {
    case LVN_ITEMCHANGED:
        // scf.cpp already narrowed this down to transitions into "selected".
        if (info.selected && m_on_selection_changed) m_on_selection_changed(info.item);
        break;
    case NM_DBLCLK:
        if (m_on_activate) m_on_activate(info.item);
        break;
    case NM_CLICK:
        if (m_on_click) m_on_click(info.item);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Appearance
// ---------------------------------------------------------------------------

void listview::set_header_visible(bool on)
{
    // LVS_NOCOLUMNHEADER hides the header row; the column itself remains and
    // still lays out the text. Pair it with one auto-width column
    // (add_column(title, 0)) to get a plain single-string list.
    set_style(on ? 0u : LVS_NOCOLUMNHEADER, LVS_NOCOLUMNHEADER);
}

void listview::set_full_row_select(bool on)
{
    m_full_row_select = on;
    apply_extended_styles();
}

void listview::set_grid_lines(bool on)
{
    m_grid_lines = on;
    apply_extended_styles();
}

void listview::set_batch_mode(bool on)
{
    m_batch_mode = on;
    if (!m_hwnd) return;

    HWND h = scl2::to_handle<HWND>(m_hwnd);
    SendMessageW(h, WM_SETREDRAW, on ? FALSE : TRUE, 0);
    if (!on) InvalidateRect(h, nullptr, TRUE);   // repaint once when unfrozen
}

} // namespace scf
