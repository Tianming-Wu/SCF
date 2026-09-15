

#include <iostream>

void scfhelp()
{
    std::cout <<
        "scfcli - command-line front-end for the scf library\n"
        "\n"
        "Usage:\n"
        "  scfcli --show <file.png|file.bmp> [scale] [--keep-aspect-ratio]\n"
        "  scfcli --show-bmp | --show-bitmap | --show-png   aliases of --show\n"
        "      shows a PNG or BMP (color or 1-bit) in a window; the format is\n"
        "      detected from the file contents, not the extension\n"
        "      scale >= 1 scales the image up; --keep-aspect-ratio / -k keeps the\n"
        "      window's aspect ratio while dragging its borders\n"
        "  scfcli --ask <content> [--title <title>]      confirmation dialog (adaptive size)\n"
        "  scfcli --confirm <content> [--title <title>]  alias of --ask; prints the clicked button\n"
        "  scfcli --tray                                system-tray icon demo (right-click for menu)\n"
        "  scfcli --help | -h                      show this help\n";
}