# Contributing to sqterm

Thank you for looking. sqterm is part of the
[squared SDK](https://squared-netizen.github.io/squared-netizen/),
maintained by one person, so a short note first saves everyone time.

- **Something large** (a new feature, a change to a format or an API):
  open an issue describing it before writing the code.
- **A fix or a small change**: a pull request from a branch is welcome.
  Say what it changes and why, and how you checked it.
- **A security issue**: never in public. See [SECURITY.md](SECURITY.md).

## Before a pull request

Build the app as the README describes (`make dev` on the phone). A change to a package recipe is checked by CI's packages workflow, which builds every package it affects; run `bash packages/build.sh NAME` locally when you can. Downloads belong only in `tools/fetch_packages.sh`.

The pull request's CI must be green before it is merged. Builds are
offline: nothing in a build may download anything.

## Licence

sqterm is under the MIT licence ([LICENSE](LICENSE)). By contributing,
you agree that your contribution is licensed under it too.
