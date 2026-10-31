{
  description = "ESP32 smartwatch + companion: firmware and VPS helper dev environment";

  inputs = {
    # Pinned: provides the Nix packaging for ESP-IDF and the Espressif toolchains.
    esp-dev.url = "github:mirrexagon/nixpkgs-esp-dev/5287d6e1ca9e15ebd5113c41b9590c468e1e001b";
    nixpkgs.follows = "esp-dev/nixpkgs";
    flake-utils.follows = "esp-dev/flake-utils";
  };

  outputs = { self, nixpkgs, esp-dev, flake-utils }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [ esp-dev.overlays.default ];
          # esptool needs python ecdsa, which nixpkgs flags as insecure.
          config.permittedInsecurePackages = [ "python3.13-ecdsa-0.19.1" ];
        };

        # Same ESP-IDF release for the ESP32-S3 watch and the ESP32 companion.
        # v5.5.5 is the version Waveshare validates its 2.06" AMOLED examples against.
        esp-idf = pkgs.esp-idf-full.override {
          rev = "v5.5.5";
          sha256 = "sha256-/xVsusdmXFMLImfKryoB5ulR09AJefzoIUFmTAb3n/s=";
          # Both targets are Xtensa; skip RISC-V/clang/gdb (gdb fails auto-patchelf on nixos-25.11).
          toolsToInclude = [ "xtensa-esp-elf" "esp-rom-elfs" ];
        };

        # idf-component-manager 2.2.2 (bundled above) aborts with MissingKconfigError while
        # solving lvgl 9.6's Kconfig-conditional dependencies. 2.4.9 handles them; it is put
        # ahead of the bundled copy on PYTHONPATH. Its runtime deps are already in the IDF env.
        idf-component-manager = pkgs.python3Packages.buildPythonPackage rec {
          pname = "idf_component_manager";
          version = "2.4.9";
          pyproject = true;
          src = pkgs.fetchPypi {
            inherit pname version;
            hash = "sha256-bziEzZ0j1kPC2q8gr/SyDP8AUiABZ0nYY42meA+OLuw=";
          };
          build-system = [ pkgs.python3Packages.setuptools ];
          doCheck = false;
          dontCheckRuntimeDeps = true;
          pythonImportsCheck = [ ];
        };
      in {
        packages.esp-idf = esp-idf;
        packages.idf-component-manager = idf-component-manager;

        devShells.default = pkgs.mkShell {
          name = "smartwatch-dev";
          buildInputs = [
            esp-idf
            pkgs.rustc
            pkgs.cargo
            pkgs.clippy
            pkgs.rustfmt
            pkgs.pkg-config
            pkgs.nodejs_22  # npx lv_font_conv (scripts/sw fonts)
            pkgs.gnumake
            pkgs.gcc
            pkgs.jq
            pkgs.zip
          ];
          shellHook = ''
            export SMARTWATCH_ROOT="$PWD"
            export PYTHONPATH="${idf-component-manager}/${pkgs.python3.sitePackages}''${PYTHONPATH:+:$PYTHONPATH}"
            # Managed components and build output stay inside the repo (gitignored).
            export IDF_COMPONENT_CACHE_PATH="$PWD/build/cache/idf-components"
            export CARGO_TARGET_DIR="$PWD/build/server"
            echo "ESP-IDF $(cat "$IDF_PATH/version.txt") ready. See README.md for build scripts."
          '';
        };

        # Lightweight shell for host-side tests and server work without ESP-IDF.
        devShells.host = pkgs.mkShell {
          buildInputs = [ pkgs.rustc pkgs.cargo pkgs.clippy pkgs.rustfmt pkgs.cmake pkgs.ninja pkgs.gcc pkgs.jq ];
        };
      });
}
