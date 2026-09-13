# SCF (Simple Framework - Cpp)

This library provides convenient functions to show simple UI components on Windows.

It is part of the [SharedCppLib2](https://github.com/Tianming-Wu/SharedCppLib2) ecosystem.


## Brief Overview

SCF provides a extremely friendly interface to show simple UI components on Windows.

It is designed to be extremely easy to use. Even a console application can add UI features in just a few lines of code that can be easily added or removed.

It is also designed to be lightweight. However it do requires SharedCppLib2, since it will make things so much easier. The consumer doesn't actually need to know about SharedCppLib2.

However, some of the features are actually built for SharedCppLib2. SharedCppLib2 should not depend on actual platform too much, so it should not include any UI features. SCF is designed to be a separate library that can be used by SharedCppLib2 users to provide UI features.


## Usage

Some features can be called within a few lines:

```cpp

auto bitmap = scl2::qrcode::make_matrix("Hello World!");
auto w = scf::showBitmap(bitmap, "QR Code");

w.wait_for_closed();

```

Others may require a few more lines, but still very simple:

```cpp

auto dlg = scf::askForConfirmation("Are you sure you want to continue?", "Confirmation");

if (dlg.result() == scf::DialogButton::Yes) {
    // User clicked Yes
} else {
    // User clicked No or closed the dialog
}

```

If you want something complex, it is also possible to create a window and add controls to it:

```cpp

auto w = scf::window(scl2::Geometry(100, 100, 400, 300), "My Window");

auto lbl = scf::label(scl2::Geometry(10, 10, 200, 30), "Hello World!");
w.add_control(lbl);

auto btn = scf::button(scl2::Geometry(10, 50, 100, 30), "Click Me");
btn.on_click([&]() {
    scf::showMessage("Button Clicked!", "Info");
});
w.add_control(btn);

w.on_close([&]() {
    scf::showMessage("Goodbye!", "Info");
});

w.show();

```

You should find the api very friendly if you had experience with Qt like libraries. However, I explicitly discarded the signal-slot machanism. I of course know how to do that, but lambda is easy enough, and just
a little bit more verbose that raw signal-slot. You can also distribute somehow in your own lambda.

Check the full [Document](/doc.md)