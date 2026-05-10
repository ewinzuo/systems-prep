{ pkgs ? import <nixpkgs> {} }:

let
  virtme-ng = pkgs.python3Packages.buildPythonApplication rec {
    pname = "virtme-ng";
    version = "1.31";
    pyproject = true;

    src = pkgs.fetchFromGitHub {
      owner = "arighi";
      repo = "virtme-ng";
      rev = "v${version}";
      hash = "sha256-0000000000000000000000000000000000000000000=";
    };

    build-system = [ pkgs.python3Packages.setuptools ];
    propagatedBuildInputs = with pkgs.python3Packages; [ argcomplete ];
    doCheck = false;
  };
in

pkgs.mkShell {
  buildInputs = with pkgs; [
    # QEMU build dependencies
    gcc
    meson
    ninja
    pkg-config
    python3
    python3Packages.distlib
    python3Packages.setuptools
    flex
    bison
    glib
    pixman
    zlib
    libslirp
    dtc

    # Kernel module build
    gnumake
    linuxPackages.kernel.dev

    # virtme-ng for quick VM testing
    python3Packages.pip
    python3Packages.virtualenv

    # VM tools
    busybox
    xorriso
    curl
    xz
  ];

  shellHook = ''
    # Install virtme-ng in a local venv if not already present
    if [ ! -d .venv ]; then
      python3 -m venv .venv
      .venv/bin/pip install virtme-ng -q
    fi
    export PATH="$PWD/.venv/bin:$PATH"
  '';
}
