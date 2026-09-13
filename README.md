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

auto bitmap = scl2::qrcode::encoder::generate("Hello World!");
auto w = scf::showBitmap(bitmap, "QR Code");

w.wait_for_closed();

```

Others may require a few more lines, but still very simple:

```cpp

auto dlg = scf::askForConfirmation(L"Are you sure you want to continue?", L"Confirmation");

switch (dlg.get()) {
case scf::DialogButton::Ok:
    // User confirmed
    break;
case scf::DialogButton::Cancel:
    // User declined
    break;
default:
    // The window was closed directly
    break;
}

```

If you want something complex, it is also possible to create a window and add controls to it:

```cpp

auto w = scf::window(scl2::Rect{100, 100, 400, 300}, L"My Window");

auto lbl = scf::new_label(scl2::Rect{10, 10, 200, 30}, L"Hello World!");
w.add_child(lbl);

auto btn = scf::new_button(scl2::Rect{10, 50, 100, 30}, L"Click Me");
btn->on_click([lbl]() {
    lbl->set_text(L"Button clicked!");
});
w.add_child(btn);

w.on_close([](scf::window&) {
    // The window is going away; release whatever you held for it.
});

w.show();
w.wait_for_closed();

```

You should find the api very friendly if you had experience with Qt like libraries. However, I explicitly discarded the signal-slot machanism. I of course know how to do that, but lambda is easy enough, and just
a little bit more verbose that raw signal-slot. You can also distribute somehow in your own lambda.

Check the full [Document](/doc.md)