# gtiup

A standalone C++ toolchain manager for [GTI](https://github.com/lang-gti-official/gti).
Install and select matching compiler, LSP, standard library, runtime and parser
from **preview**, **nightly**, **beta**, **stable**, or a pinned published version.

The manager has its own builds, tests, version numbers and GitHub downloads.
It does not build GTI or LLVM. Installing or running the released manager needs
neither Python nor a compiler. Python is used only by this repository's tests
and release packaging.

## Install

On macOS arm64 or Linux x64:

```sh
curl -fsSL https://gtireference.internal.howetotech.dev/gtiup/install.sh | sh
```

This installs gtiup and the **nightly** GTI toolchain, then configures PATH for
bash, zsh or sh. Open a new terminal afterward and run `gti --version`.
Existing shell configuration is preserved; repeating the installer does not
add duplicate PATH entries. No sudo is required.

Choose another channel during installation:

```sh
curl -fsSL https://gtireference.internal.howetotech.dev/gtiup/install.sh | sh -s -- --default-toolchain preview
```

Use `nightly`, `beta`, `stable`, or an explicit published version in place of
`preview`. Unpublished channels fail clearly instead of falling back. Use
`--default-toolchain none` for manager-only installation, and `--no-modify-path`
to manage shell configuration yourself (also for shells other than bash/zsh/sh).
The script creates `~/.gtiup/env`, sources it from `.profile` and the selected
shell's startup files, and leaves unrelated settings intact. You can inspect the
script before executing it by downloading it to a file first.

Set `GTIUP_HOME` to change the install location or `GTIUP_VERSION=v0.1.0` to pin
the manager release. Re-running the default installer updates the manager and
selects nightly again; pass your desired channel explicitly to retain another
selection, or `--default-toolchain none` to update only the manager.

For manual installation, download `gtiup-darwin-arm64` or `gtiup-linux-x64`, its
`.sha256` and `.licenses.txt` from [Releases](https://github.com/lang-gti-official/gtiup/releases).
Verify the SHA256, make the binary executable, and run `./gtiup-<platform> self-install`.
Linux downloads are statically linked musl executables; macOS downloads use only
Apple-provided system libraries. HTTPS uses the host's CA certificates.

The installer needs `curl` and `sha256sum` or `shasum`. The installed native
manager performs HTTPS and archive extraction itself; it does not shell out to
Python, curl, tar or a compiler.

## Select a toolchain

```sh
gtiup default nightly
gtiup install preview
gtiup install beta
gtiup install stable
gtiup install 0.12.0          # a published immutable stable version
gtiup install 0.13.0-beta.2  # a published beta version
gtiup update nightly
gtiup list
gtiup show
gti +preview build
gtiup run nightly gti_lsp --version
gtiup which gti_lsp
```

Versions above illustrate syntax, not a promise that those releases exist.
Channels that have not been published fail clearly. A yanked stable or withdrawn
beta is refused; the manager never silently substitutes another release.

With gtiup 0.2.0 or later, put a `gti-toolchain.toml` file in your project:

```toml
[toolchain]
channel = "nightly"
```

`channel` accepts `preview`, `nightly`, `beta`, `stable`, or a published pinned
version such as `0.3.0-beta.2`. Comments and standard TOML string syntax work.
Only `toolchain.channel` is supported: settings such as Rustup's `components`,
`targets`, `profile`, or `path` are rejected rather than silently ignored.

The plain-text `gti-toolchain` file remains supported:

```text
nightly
```

Precedence is explicit `+channel`/`gtiup run`, then `GTIUP_TOOLCHAIN`, then the
nearest ancestor containing either file, then the configured default. If both
files exist in the same directory, `gti-toolchain` wins for compatibility with
Rustup's rule. That filename also accepts TOML; `gti-toolchain.toml` requires TOML.
Invalid project files produce an error instead of falling back to a parent or
the default. `gti` and `gti_lsp`
in `~/.gtiup/bin` are manager proxies which execute the selected real binary
with its original resource location. Selection never downloads implicitly.
The compiler itself does not call gtiup.

`gtiup uninstall CHANNEL` removes a selection (select another default first).
Updates and uninstall retain immutable old directories for running compiler/LSP
processes. Close those processes before explicitly running `gtiup gc` to reclaim
unreferenced toolchains. Unmanaged directories are retained.

## Channels and trust

Compiler archives come from `lang-gti-official/gti`; manager binaries come from
`lang-gti-official/gtiup`. These are independent release lifecycles. Preview is
an early combined toolchain and retains its inclusion manifest; it is not a
nightly/stable qualification. The manager validates SHA256, archive paths and
entry types, rolling source identity, preview metadata, version identity and
release revocations before atomically changing a selection. A failed update
preserves the current selection.

Checksums and metadata are fetched over authenticated HTTPS from the official
GitHub repositories. This does not independently authenticate a compromised
release publisher. Releases also carry GitHub provenance attestations; optional
verification is available through `gh attestation verify`. gtiup does not yet
perform attestation verification itself.

## Neovim

Install the GTI Neovim plugin from `lang-gti-official/gti`, and enable its managed
mode with `vim.g.gti_toolchain = "gtiup"`. Configure the plugin without its legacy
compiler-download build hook. Keep gtiup on Neovim's PATH or set `GTIUP_PATH`.
The plugin resolves the selected project's matching LSP and parser through
`gtiup which`; it does not install another compiler or LSP. This requires the
managed-mode integration in the GTI plugin. See that repository's current setup
instructions for its availability and configuration.

## Development

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

C++17, CMake 3.24+, zlib development files, and Python 3.9+ for tests are needed.
Linux development builds also need OpenSSL development files. macOS uses its SDK
libcurl and CommonCrypto. CMake downloads checksum-pinned JSON/libarchive sources
and, on Linux, libcurl. `scripts/linux-static.sh` builds the release in Alpine;
`GTIUP_SANITIZERS=ON` enables ASan/UBSan for development validation.

Tests use a separately compiled, loopback-only test client and temporary homes.
The production binary contains no test endpoint override. CI checks both native
platforms, static Linux packaging, shell syntax, and sanitizer runs. It never
checks out or builds the GTI compiler.

To release: update the CMake project version through a PR, let gtiup CI pass,
and tag that exact commit `vX.Y.Z`. The tag workflow rebuilds/tests the small
manager, checks both platform inventories, generates provenance and creates an
immutable GitHub release. Existing releases are never overwritten. Toolchain
preview/nightly/beta/stable publication stays in the compiler repository.
