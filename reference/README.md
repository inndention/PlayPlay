# reference/

## electron-src/

The source of the original PLAY PLAY operator app (Electron, React and TypeScript, about 400 files) that the native C++/Qt
application re-creates. It is kept here as a **non-buildable feature reference**: there is no `package.json`, bundler
configuration or lockfile, nothing in the CMake build uses it, and it imports npm packages that are not vendored. Read it
to see how a feature was meant to behave, not to run it.

- Some files still contain defaults that belonged to the original station (for example `getstarnews.com` endpoints,
  local-market names and example copy). Treat them as sample data.
- The same licence situation applies as for the rest of the repository: none has been added yet, so all rights are
  reserved (see the [README](../README.md#licence)).
- The matching inventories are [docs/reference-feature-inventory.md](../docs/reference-feature-inventory.md) and
  [docs/reference-playplaypy-inventory.md](../docs/reference-playplaypy-inventory.md); screenshots of its UI are in
  [docs/ui-reference/](../docs/ui-reference/).
