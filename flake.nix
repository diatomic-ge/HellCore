{
  description = "The HellCore MOO engine and database.";

  # Flake compatibility functions.
  inputs.flake-compat = {
    url = "github:edolstra/flake-compat";
    flake = false;
  };

  # Flake utility functions.
  inputs.flake-utils.url = "github:numtide/flake-utils";

  # Nixpkgs.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";

  # The outputs this flake provides.
  outputs =
    { self
    , nixpkgs
    , flake-utils
    , ...
    }:
    flake-utils.lib.eachDefaultSystem (system:
    let
      # The packages we use to provide gcc, etc.
      pkgs = import nixpkgs {
        inherit system;
      };
    in
    rec {
      # The contents of a shell for developing on hellcore.
      devShell = pkgs.mkShell {
        # Include all the dependencies of hellcore in the shell, to allow easy
        # manual building.
        inputsFrom = [
          packages.hellcore
        ];
      };

      # The default package to build.
      defaultPackage = packages.hellcore;

      packages = flake-utils.lib.flattenTree {
        # The hellcore package.
        hellcore = pkgs.stdenv.mkDerivation rec {
          # The package name, version, and full package-version for this
          # hellcore build.
          pname = "hellcore";
          version = self.lastModifiedDate;
          name = "${pname}-${version}";

          src = self;

          # The build-time inputs needed to build hellcore.
          nativeBuildInputs = with pkgs; [
            # Include the autoconf-archive macros so that aclocal can update the
            # openssl macro.
            # You don't need this to build hellcore, or its build system, but
            # having it will let aclocal find newer versions of any macros we
            # use from it.
            autoconf-archive

            # This automatically regenerates the autoconf and automake build
            # system.
            autoreconfHook

            # The bison parser generator.
            bison

            # The flex lexxer generator.
            flex

            # The gperf perfect hash generator for recognizing keywords.
            gperf

            # A more modern libcrypt.
            # This is necessary, since libcrypt might be being phased out of
            # glibc, and we won't get a libcrypt for free from stdenv.
            # We enable all hashes, so that databases with old DES hashes don't
            # break.
            (libxcrypt.override { enableHashes = "all"; })

            # OpenSSL, for TLS support.
            openssl.dev

            # A helper for finding the right compiler flags for libraries, used
            # to help build against OpenSSL.
            pkg-config
          ];

          # Extra flags to pass to ./configure.
          configureFlags = [
            # Show the full compiler calls, rather than just CC <file> for
            # logging.
            "--disable-silent-rules"
          ];
        };
      };
    }
    );
}
