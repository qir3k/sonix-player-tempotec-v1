# Branches, tests and releases

This repository has two branch roles, and no third one.

| Branch | Purpose | What a push runs |
|---|---|---|
| `dev` | Development and integration; the branch everything is written and tested on. | The host player build and every test suite (`tests.yml`). No firmware image. |
| `personal` | Releases for the maintainer's own V1, and for anyone flashing the published images. | The same tests first, then the firmware build, a new version and a GitHub release with `v1.upt` and `v1_md5.txt`. |

`main` is not used. Nothing is merged into it and no workflow builds from it.

## Writing a change

Work on `dev`. A push there is the fast feedback loop: it compiles the host player, runs the headless suites, and compiles and runs the target-only modules that have host suites. If that run is red, the branch is not ready to merge.

```bash
git switch dev
# ... work ...
git push origin dev
```

## Checks before a release

The same suites run in two places, from one definition (`.github/workflows/tests.yml`):

- on every push to `dev` and `personal`, and on pull requests touching the player, the tests or the packer;
- as the first job of the firmware workflow — `build-tempotec-v1.yml` calls the reusable workflow and its firmware job `needs` it, so **a red suite stops the release before anything is packaged**.

To run them by hand on a Linux host, from the repository root:

```bash
make -C sonix-player/tests all
```

That is the same list, in the same order, as the workflow. Every target can also
be built and run on its own, which is what to do while working on one thing.

[sonix-player/tests/README.md](../sonix-player/tests/README.md) says what each one pins.

## Personal firmware and release

Merge `dev` into `personal` when the changes have been tested. The push:

1. runs the test suites (and stops here if one fails);
2. builds the firmware from the merged commit, with the MIPS toolchain from `sonix-player/rockboxdev/`;
3. stamps the image with a new version, this repository as its update source, and the personal fork in its credits;
4. creates a release whose name is the new version, whose body lists the commits since the last one, and whose assets are `v1.upt` and `v1_md5.txt`.

The version normally advances by one patch. A commit message can request a minor or major bump. The run's Actions artifact additionally carries `BUILD-REPORT.txt`, the stock and output validation reports, and the player binaries; the build report names the exact commit that was packaged.

Download `v1.upt` and `v1_md5.txt` **from the same release**, and check the report before installing.

### A manual run

**Build TempoTec V1 firmware** can be dispatched for a branch, tag or commit. A manual run asks for:

- the **stock firmware URL** — the official `v1.upt` the image is built on;
- the **community-build acknowledgement** — the run refuses to start without it;
- optionally **`build_ref`**, the ref to build, and **`publish_release_tag`**, an existing release to upload the built image to. The workflow verifies that the tag names the commit it built before replacing any asset.

A manual run without `publish_release_tag` produces an Actions artifact and nothing else. Tests run first here too.

## Before installing

These are unofficial community builds for the TempoTec Variations V1. The workflow checks the package and confirms the stock kernel is unchanged, but it does not install the image or test it on physical hardware. Keep the official V1 `v1.upt` and its matching checksum available for recovery. Install the firmware and checksum together, and do not use the V1 package on another model.
