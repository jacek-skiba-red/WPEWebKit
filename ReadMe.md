# WebKit

[WebKit](https://github.com/webkit/webkit) is a cross-platform web browser engine. On iOS and macOS, it powers Safari, Mail, iBooks, and many other applications.

# WPEWebKit

WPE WebKit (formerly WebKitForWayland) is a WebKit port intended to allow embedders to create simple and performant systems based on Web platform technologies. It is designed with hardware acceleration in mind, leveraging common 3D graphics APIs for best performance.

You can find more information about WPE at [https://wpewebkit.org](https://wpewebkit.org).

## Clarifying: "Upstream WPE" vs "Downstream WPE"

This section will clarify the difference between upstream WPE ([https://github.com/WebKit/WebKit](https://github.com/WebKit/WebKit)) and downstream WPE (_this_ repository).

> The upstream version

- The [_upstream_ version](https://github.com/WebKit/WebKit) is more generic, device-agnostic, and free of customizations.

> The downstream version (this repository)

- This repository - `WPEWebKit` - the _downstream_ version - is optimized for Raspberry Pi and set-top boxes.

- It includes customizations for Broadcom devices (amongst other devices as well), and it also includes a better integration with Thunder (aka WPEFramework).

## Getting Started with WPEWebKit

See ["Getting Started with WPE"](https://wpewebkit.org/about/explore-wpe.html).

There is an [Frequently Asked Questions section](https://wpewebkit.org/about/faq.html) as well.

Tarball releases are [here](https://wpewebkit.org/release/).

## Building WPE

- Clone this repository.
- Clone [the buildroot repository](https://github.com/WebPlatformForEmbedded/buildroot).
- See also [https://github.com/WebPlatformForEmbedded/meta-wpe](https://github.com/WebPlatformForEmbedded/meta-wpe).

You can use the [buildroot repository](https://github.com/WebPlatformForEmbedded/buildroot) to build WPE. There are more details on the buildroot repository itself, as well as in [Igalia meta-webkit wiki](https://github.com/Igalia/meta-webkit/wiki/WPE), which gives more details.

(On Windows, follow the [instructions on our website](https://webkit.org/webkit-on-windows/))


## Contribute

Congratulations! You’re up and running. Now you can begin coding in WPE WebKit and contribute your fixes and new features to the project.
