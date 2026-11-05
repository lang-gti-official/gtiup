# gtiup contribution guidance

This is the standalone C++ GTI toolchain manager, not the GTI compiler repository.
Canonical repository: https://github.com/lang-gti-official/gtiup.

Use a task branch and PR after initial repository bootstrap. Preserve unrelated
work. Validate with this repository's CMake/CTest and platform CI; never invoke
the GTI compiler build, compiler test suite or compiler merge queue for manager
changes. User authorization is required for installing into their real environment.
Temporary test homes and fixtures are appropriate validation.

Keep the published binary independent of Python and the GTI compiler. Preserve
nightly, beta, stable, explicit-version and linked-toolchain selection (the
retired preview channel is refused, not substituted); atomic activation;
checksum/source/version checks; safe archive extraction; revocation failures;
and project-aware compiler/LSP proxies. Test failure paths, not only success.
No shell interpolation of downloaded metadata. No insecure TLS or production
test-endpoint overrides. Keep native dependencies pinned and licensed.

Manager releases belong here. Compiler archives and release-status belong to
lang-gti-official/gti. Never alter a compiler release or bypass its admission as
part of a manager change. Manager CI gates only manager releases. Publish an
immutable vX.Y.Z manager release only after its exact-source platform tests pass.
