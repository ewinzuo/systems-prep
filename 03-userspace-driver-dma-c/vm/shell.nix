{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = with pkgs; [
    # QEMU build dependencies
    gcc
    meson
    ninja
    pkg-config
    python3
    flex
    bison
    glib
    pixman
    zlib
    libslirp
    dtc

    # Utilities
    curl
    xz
  ];
}
