# Firmware branches and releases

This repository has three branch roles. Merge finished work from `dev` into the
branch whose firmware you want to build.

| Branch | Purpose | Firmware behavior |
|---|---|---|
| `dev` | Ongoing development and integration | No TempoTec firmware build runs on pushes to this branch. |
| `personal` | Firmware for the maintainer's own device and personal updates | Each push starts a firmware build, increments the version by default, and publishes a GitHub release with `v1.upt` and `v1_md5.txt`. |
| `main` | Shareable, general-purpose version | A push creates an Actions artifact. Publishing a GitHub release is a separate, optional action. |

## Personal firmware

Merge `dev` into `personal` when the changes are ready for the personal device.
The workflow builds the resulting `personal` commit, stamps the image with its
new version and this repository as its update source, then creates a release.
The release contains `v1.upt` and the matching `v1_md5.txt`; its notes list the
commits included in that version. The version normally advances by one patch
for each push. A commit message can request a minor or major bump.

Download both firmware files from the corresponding release. The Actions
artifact for the same run also includes `BUILD-REPORT.txt`, stock and output
validation reports, and the player binaries. Check the report's commit before
installing an artifact.

## Shareable firmware from `main`

Merge `dev` into `main` when preparing the shareable version. The push produces
an Actions artifact named after the built commit; it does not automatically
create or update a release. If you want to publish a shareable release, tag the
intended `main` commit or manually run **Build TempoTec V1 firmware** with that
commit and the matching `publish_release_tag`. The workflow verifies that the
release tag points to the commit it built before uploading `v1.upt` and
`v1_md5.txt`.

The firmware workflow can also be run manually for a branch, tag or commit.
Manual runs require an official stock firmware URL and the community-build
acknowledgement. A run without a release tag creates only an Actions artifact.
`dev` is deliberately not a push-triggered firmware source.

## Before installing

These are unofficial community builds for the TempoTec Variations V1. The
workflow checks the package and confirms the stock kernel is unchanged, but it
does not install the image or test it on physical hardware. Keep the official
V1 `v1.upt` and its matching checksum available for recovery. Install the
firmware and checksum together, and do not use the V1 package on another model.
