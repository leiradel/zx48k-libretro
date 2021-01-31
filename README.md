# zx48k-libretro

A simple [ZX Spectrum 48K](https://en.wikipedia.org/wiki/ZX_Spectrum#ZX_Spectrum_16K/48K) [Libretro](https://www.libretro.com/) core based on the [chips](https://github.com/floooh/chips) toolbox. The core can only open [Z80 files](https://worldofspectrum.org/faq/reference/z80format.htm), and doesn't support save states.

## Build

A very simple `Makefile` is provided, type `make` to create the core. If that doesn't work please submit a PR, the only file that has to be compiled and linked is `src/main.c`.

## License

`src/main.c` is MIT, the other files in `src/` are licensed under the Zlib license.
