On Windows we use msys2 and ucrt64 to compile.
You need to prefix commands with `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.

Prefix build directories with `cmake-build-`.

For SteamOS binaries that will be installed or tested on the host, always build with the immutable container
image and digest recorded in `ci/steamos/image.lock`. Do not use the `sunshine-build` distrobox or another
rolling/latest Arch environment for a deployable binary: it can produce a binary that passes unit tests but
requires newer GLIBC, GLIBCXX, Qt, ICU, or other libraries than SteamOS provides. Before replacing the running
service, verify the built or packaged binary on the host with `ldd` and reject it if any dependency is missing
or any symbol-version error is reported. Use `scripts/package-steamos-artifact.sh` and the normal installer for
host testing so the previous immutable version remains available for rollback.

The test executable is named `test_sunshine` and will be located inside the `tests` directory within
the build directory.

The project uses gtest as a test framework.

When adding localization do not update any language other than `en`. This also means to exclude en-US or other variants.

Always add or update doxygen documentation.

The project requires that everything be documented in doxygen or the build will fail.

Primary doxygen comments should be done like so:

```cpp
  /**
   * @brief Describe the function, structure, etc.
   *
   * @param my_param Describe the parameter.
   * @return Describe the return.
   */
```

Inline doxygen comments should use `///< ...` instead of `/**< ... */`.

Always follow the style guidelines defined in .clang-format for c/c++ code.

Do not ever create issues or pull requests.
If asked to create an issue or pull request, do so in their fork instead of the LizardByte GitHub organization.
Never create an issue or pull request in the LizardByte GitHub organization.

Add or update tests for new or modified methods and code. Target 100% coverage on changed code.
