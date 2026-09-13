// scfcli -- command-line front-end for the scf library.

#include "scf.hpp"

#include <SharedCppLib2/bitmap.hpp>
#include <SharedCppLib2/stringlist.hpp>
#include <SharedCppLib2/fileio.hpp>

#include <cstdio>
#include <stdexcept>

#include "scf_tray.hpp"   // tray demo (brings in windows.h)

void scfhelp(); // in scfhelp.cpp

// scfcli --show-bmp <file.bmp> [scale] [--keep-aspect-ratio]
// Reads a BMP file (color or 1-bit), shows it in a window scaled by `scale`,
// and returns once the user closes the window. With --keep-aspect-ratio the
// window keeps its client aspect ratio while the user drags its borders.
static int showBmpCommand(const scl2::wstringlist& args)
{
    if (args.size() < 2) {
        std::fwprintf(stderr, L"scfcli: --show-bmp needs a file path\n");
        return 1;
    }

    const std::wstring path = args[1];
    int scale = 1;
    if (args.size() > 2) {
        try {
            scale = std::stoi(args[2]);
        } catch (const std::exception&) {
            std::fwprintf(stderr, L"scfcli: invalid scale '%ls'\n", args[2].c_str());
            return 1;
        }
        if (scale < 1) scale = 1;
    }

    scl2::bytearray data;
    try {
        data = scl2::readFile(path);
    } catch (const std::exception& e) {
        std::fwprintf(stderr, L"scfcli: cannot read '%ls': %hs\n", path.c_str(), e.what());
        return 1;
    }
    if (data.empty()) {
        std::fwprintf(stderr, L"scfcli: '%ls' is empty or unreadable\n", path.c_str());
        return 1;
    }

    const bool keep = args.contains(L"--keep-aspect-ratio") || args.contains(L"-k");

    // Try color (8/24/32-bit BI_RGB) first, then fall back to 1-bit.
    try {
        scl2::bitmap<scl2::rgba8> bmp = scl2::bitmap<scl2::rgba8>::fromBmp(data);
        if (bmp.width() == 0 || bmp.height() == 0) throw std::invalid_argument("empty");
        scf::window win = scf::showBitmap(bmp, scale, scl2::wstr_to_str(path));
        if (keep) win.set_keep_aspect_ratio(true);
        win.wait_for_closed();
        return 0;
    } catch (const std::exception&) {
        // Not a color BMP (e.g. 1-bit); try monochrome below.
    }

    try {
        scl2::bitmap_1c bmp = scl2::bitmap_1c::fromBmp(data);
        if (bmp.width() == 0 || bmp.height() == 0) throw std::invalid_argument("empty");
        scf::window win = scf::showBitmap(bmp, scale, scl2::wstr_to_str(path));
        if (keep) win.set_keep_aspect_ratio(true);
        win.wait_for_closed();
        return 0;
    } catch (const std::exception& e) {
        std::fwprintf(stderr, L"scfcli: '%ls' is not a supported BMP: %hs\n", path.c_str(), e.what());
        return 1;
    }
}

// scfcli --tray
// Minimal system-tray demo: a small window plus a tray icon.
//  - right-click the icon: popup the SAME held menu each time (built once)
//      * Hide window -> hides; double-click the icon restores it
//      * Balloon -> toast; Quit -> close
static int trayCommand()
{
    scf::window w(scl2::Rect{0, 0, 220, 140}, L"scf tray demo");
    w.show();

    scf::icon ic(CopyIcon(LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION))));
    scf::tray_icon ti(w, ic, L"scf tray demo");

    // The context menu is built once and held on the heap; TrackPopupMenu is
    // non-destructive, so the same HMENU can be popped again on every
    // right-click without rebuilding.
    auto m = std::make_shared<scf::menu>();
    m->item(L"Hide window", 1);
    m->separator();
    m->item(L"Balloon", 2);
    m->item(L"Quit", 3);

    // Typical tray behaviour: double-click restores a hidden window.
    ti.on_double_click([&w] { w.show(); });

    ti.on_right_click([&w, &ti, m] {
        const unsigned int cmd = ti.popup_menu(*m);
        if (cmd == 1) w.hide();
        else if (cmd == 2) ti.show_balloon(L"Hi", L"The tray icon works.");
        else if (cmd == 3) w.close();
    });

    if (!ti.show()) {
        std::fwprintf(stderr, L"scfcli: could not add the tray icon\n");
        return 1;
    }
    w.wait_for_closed();
    return 0;
}

// scfcli --ask <content> [--title <title>]
// Shows a confirmation dialog whose size adapts to the content, then prints
// the button that closed it (Ok / Cancel / Closed).
static int askCommand(const scl2::wstringlist& args)
{
    if (args.size() < 2) {
        std::fwprintf(stderr, L"scfcli: --ask needs content text\n");
        return 1;
    }
    const std::wstring content = args[1];

    size_t ti = args.find(L"--title");
    if (ti >= args.size()) ti = args.find(L"-t");
    std::wstring title = L"Confirm";
    if (ti < args.size() && ti + 1 < args.size()) title = args[ti + 1];

    scf::dialog<scf::DialogButton> dlg = scf::askForConfirmation(content, title);
    const scf::DialogButton r = dlg.get();

    const char* name = "Other"; 
    switch (r) {
        case scf::DialogButton::Ok:     name = "Ok"; break;
        case scf::DialogButton::Cancel: name = "Cancel"; break;
        case scf::DialogButton::Closed: name = "Closed"; break;
        default: break;
    }
    std::printf("dialog result: %s\n", name);
    return 0;
}

// wmain: wide argv so Chinese command-line arguments arrive intact no matter
// the console codepage (no ANSI -> UTF-8 guessing on the caller side).
int wmain(int argc, wchar_t* argv[])
{
    scl2::wstringlist args(argc, argv, 1);

    if (args.empty()) { return 1; }

    if (args.contains(L"--help") || args.contains(L"-h")) {
        scfhelp();
        return 0;
    }

    if (args[0] == L"--show-bmp" || args[0] == L"--show-bitmap") {
        return showBmpCommand(args);
    }

    if (args[0] == L"--ask" || args[0] == L"--confirm") {
        return askCommand(args);
    }

    if (args[0] == L"--tray") {
        return trayCommand();
    }

    std::fwprintf(stderr, L"scfcli: unknown command '%ls' (try --help)\n", args[0].c_str());
    return 1;
}
