# Contributing

Contributions are welcome through GitHub issues and pull requests. Keep discussion and project
content in English.

## Development workflow

1. Create a focused branch.
2. Build and run the tests using the commands in the README.
3. Format changed C++ files with `clang-format`.
4. Run `clang-tidy` against the generated compilation database when changing production code.
5. Explain behavior changes and tests in the pull request.

Do not add live-network dependencies to the default test suite. Network tests must use the
`network` CTest label and remain opt-in.

By contributing, you agree that your contribution is licensed under GPL-3.0-or-later.
