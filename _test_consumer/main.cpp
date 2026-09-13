#include <scf/scf.hpp>
#include <scf/scf_tray.hpp>
#include <SharedCppLib2/bitmap.hpp>

int main() {
    // API smoke test: exercises the whole public control surface so every
    // exported symbol compiles and links against scf.dll. The window is never
    // shown, so the worker thread never starts and destruction is safe.

    scf::window w(scl2::Rect{0, 0, 320, 200});

    // button
    auto btn = scf::new_button(scl2::Rect{10, 10, 90, 26}, L"Click");
    btn->on_click([] {});

    // label: default alignment is left-top; override it freely
    auto lbl = scf::new_label(scl2::Rect{110, 10, 200, 26}, L"Hello");
    lbl->set_alignment(scl2::Alignment::Center);
    lbl->set_alignment(scl2::Alignment::Right | scl2::Alignment::VCenter);
    lbl->set_wrap(true);
    lbl->set_text(L"Centered");
    (void)lbl->get_text();

    // checkbox: pre-creation state must be remembered and applied later
    auto cb = scf::new_checkbox(scl2::Rect{10, 50, 120, 24}, L"Enable X");
    cb->set_checked(true);
    const bool pre = cb->is_checked();
    cb->set_checked(false);
    cb->on_click([] {});

    // textbox: extra events + password mode
    auto tb = scf::new_textbox(scl2::Rect{10, 90, 200, 24}, L"initial");
    tb->set_read_only(true);
    tb->set_read_only(false);
    tb->set_text(L"typed");
    tb->on_change([] {});
    tb->on_update([] {});
    tb->on_setfocus([] {});
    tb->on_killfocus([] {});
    tb->on_enter([] {});
    tb->set_password(true);
    tb->set_password(false);

    // groupbox: pure frame
    auto gb = scf::new_groupbox(scl2::Rect{10, 130, 180, 24}, L"Frame");
    w.add_child(gb);

    // radiogroup: frame + mutually-exclusive radios
    auto grp = scf::new_radiogroup(scl2::Rect{10, 130, 180, 100}, L"Pick");
    grp->add(L"Alpha");
    grp->add(L"Beta");
    grp->add(L"Gamma");
    grp->select(1);                    // pre-creation selection
    grp->on_select([](int) {});
    const int sel = grp->selected_index();
    w.add_child(grp);

    w.add_child(btn);
    w.add_child(lbl);
    w.add_child(cb);
    w.add_child(tb);

    // window-state callbacks + frameless drag zone (window never shown)
    w.on_resize([](scf::window&, int, int) {});
    w.on_move([](scf::window&, int, int) {});
    w.on_dpichange([](scf::window&, int) {});
    w.set_drag_zone(scl2::Rect{0, 0, 320, 40});
    w.set_keep_aspect_ratio(true);
    w.set_keep_aspect_ratio(false);
    w.resize(300, 160);       // pre-show: stored in geometry
    w.center_window();        // pre-show: no-op (creation centers)

    // WindowFlags ctor: frameless + right-click-exit (constructed, never shown)
    scf::window w2(scl2::Rect{0, 0, 200, 100}, L"FL",
                   scf::WindowFlags::Frameless | scf::WindowFlags::rightClickExit);
    w2.resize(400, 300);      // pre-show: stored in geometry
    (void)w2;

    // A control can reach (and close) its owner window; safe on an unshown
    // window (no HWND yet, so close() just sets the flag).
    btn->close_owner();

    // Force-link the dialog factory export (wstring overload).
    using ConfirmFn = scf::dialog<scf::DialogButton>(*)(const std::wstring&, const std::wstring&);
    ConfirmFn ask = &scf::askForConfirmation;
    (void)ask;

    // Tray API: constructing on an unshown window is safe (no HWND, so
    // show() returns false and the message hook is unregistered in the dtor).
    {
        scf::menu m;
        m.item(L"Show", 1);
        m.separator();
        m.item(L"Quit", 2);
        scf::icon ic;   // empty icon is fine for a link/construction check
        scf::tray_icon ti(w, ic, L"scf smoke");
        ti.on_click([] {});
        ti.on_double_click([] {});
        ti.on_right_click([] {});
        ti.on_balloon_click([] {});
        ti.set_tooltip(L"new tooltip");
        ti.show_balloon(L"title", L"text");
        const bool shown = ti.show();   // false: window not shown yet
        (void)shown;
    }

    // dummyWindow: constructor must auto-start the worker and create the
    // hidden OS window (no show() needed). Scoping it in/out must join the
    // worker cleanly; start() is idempotent so calling it again is a no-op.
    {
        scf::dummyWindow host;                  // hidden message-pump window
        host.start();                           // no-op (already started)
        (void)host;                             // dtor joins the worker thread
    }

    // FixedSize: the constructor flag and the pre-show setter are both
    // accepted; programmatic resize() stays available either way.
    {
        scf::window wf(scl2::Rect{0, 0, 240, 120}, L"Fixed",
                       scf::WindowFlags::FixedSize);
        wf.set_fixed_size(false);
        wf.set_fixed_size(true);
        wf.resize(300, 150);
        wf.set_keep_aspect_ratio(false);
        (void)wf;
    }

    // listview: the whole model can be built before show() (it is replayed at
    // creation), plus the selection/event surface.
    {
        auto lv = scf::new_listview(scl2::Rect{10, 10, 260, 120});
        lv->add_column(L"Name", 120);
        lv->add_column(L"PID", 50, scf::column_alignment::right);

        const int r0 = lv->add_row(L"explorer.exe", 0x1234);
        lv->add_row(L"notepad.exe");
        lv->set_cell(r0, 1, L"1234");
        (void)lv->get_cell(r0, 0);
        lv->set_row_data(1, 0x5678);
        (void)lv->row_data(1);
        (void)lv->row_count();
        (void)lv->column_count();

        lv->set_multi_select(true);
        lv->set_multi_select(false);
        lv->set_full_row_select(false);
        lv->set_full_row_select(true);
        lv->set_grid_lines(true);
        lv->set_batch_mode(true);
        lv->set_batch_mode(false);
        lv->select_row(1);
        (void)lv->selected_row();
        (void)lv->selected_rows();
        lv->ensure_visible(1);

        lv->on_selection_changed([](int) {});
        lv->on_activate([](int) {});
        lv->on_click([](int) {});

        lv->remove_row(0);
        w.add_child(lv);
        lv->clear();          // detach-safe: the window keeps it alive
    }

    // Pre-creation checkbox state and radio selection must be remembered.
    return (pre && sel == 1) ? 0 : 1;
}
