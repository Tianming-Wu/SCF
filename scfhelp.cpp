

#include <iostream>

void scfhelp()
{
    std::cout <<
        "scfcli - command-line front-end for the scf library\n"
        "\n"
        "Usage:\n"
        "  scfcli --show-bmp <file.bmp> [scale] [--keep-aspect-ratio]  show a 1-bit BMP\n"
        "  scfcli --show-bitmap <file.bmp> [scale] [--keep-aspect-ratio] alias of --show-bmp\n"
        "      scale >= 1 scales the bitmap up; --keep-aspect-ratio / -k keeps the\n"
        "      window's aspect ratio while dragging its borders\n"
        "  scfcli --ask <content> [--title <title>]      confirmation dialog (adaptive size)\n"
        "  scfcli --confirm <content> [--title <title>]  alias of --ask; prints the clicked button\n"
        "  scfcli --tray                                system-tray icon demo (right-click for menu)\n"
        "  scfcli --help | -h                      show this help\n";
}