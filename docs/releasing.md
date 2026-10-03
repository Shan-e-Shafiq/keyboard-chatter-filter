# Releasing

Releases are built and published entirely by GitHub Actions on GitHub-hosted runners, which are free
for public repositories. Nothing is built on a developer machine.

## Cutting a release

1. Update `VERSION` in `project(...)` in `CMakeLists.txt` and the `CHANGELOG.md` entry.
2. Merge to `main`, wait for CI to pass.
3. Tag and push (do not create the release in GitHub's web UI first; if one exists, the workflow
   attaches the files to it):

   ```sh
   git tag v0.1.0
   git push origin v0.1.0
   ```

4. `.github/workflows/release.yml` then:
   * checks that the tag matches the CMake project version;
   * builds Release binaries and runs the full test suite on every target:

     | Asset | Runner | Notes |
     |---|---|---|
     | `keyboard-chatter-filter-macos-arm64.tar.gz` | `macos-15` | |
     | `keyboard-chatter-filter-macos-x86_64.tar.gz` | `macos-15-intel` | |
     | `keyboard-chatter-filter-linux-x86_64.tar.gz` | `ubuntu-24.04` | static musl build in Alpine; also runs the kernel integration tests |
     | `keyboard-chatter-filter-linux-arm64.tar.gz` | `ubuntu-24.04-arm` | static musl build in Alpine |
     | `keyboard-chatter-filter-windows-x86_64.zip` | `windows-2022` | MSVC (newest Visual Studio on the image), static CRT |
     | `keyboard-chatter-filter-windows-arm64.zip` | `windows-11-arm` | MSVC (newest Visual Studio on the image), static CRT |

   * signs the macOS and Windows binaries when signing secrets are configured (see below);
   * attaches `install.sh`, `install.ps1`, `uninstall.sh` and `uninstall.ps1`, pinned to the
     repository;
   * writes `SHA256SUMS` for all assets;
   * creates a [build provenance attestation](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations)
     for every asset (`actions/attest-build-provenance`);
   * publishes the GitHub Release with generated notes.

All actions used run on Node 24 (GitHub removed Node 20 from hosted runners in September 2026).
`macos-15-intel` is the Intel runner; GitHub has announced the end of x86_64 macOS runners for
around August 2027, after which the Intel build should be cross-compiled on Apple silicon.

Running the workflow manually (`workflow_dispatch`) builds and uploads the artefacts to the run
without publishing a release, which is useful to test the pipeline.

Asset names contain no version number, so `https://github.com/<owner>/<repo>/releases/latest/download/<asset>`
always points at the newest release; the installers rely on this.

## Code signing (optional, recommended for production)

Without secrets, macOS binaries are **ad-hoc** signed and Windows binaries are unsigned; integrity
is still guaranteed by the checksums and attestations.

### macOS: Developer ID + notarization

Requires a paid Apple Developer Program membership. Add these repository secrets:

| Secret | Value |
|---|---|
| `MACOS_CERTIFICATE_P12_BASE64` | base64 of the exported *Developer ID Application* certificate + key (.p12) |
| `MACOS_CERTIFICATE_PASSWORD` | the .p12 password |
| `MACOS_SIGNING_IDENTITY` | e.g. `Developer ID Application: Your Name (TEAMID)` |
| `MACOS_NOTARY_APPLE_ID` | Apple ID used for notarization |
| `MACOS_NOTARY_PASSWORD` | an app-specific password for that Apple ID |
| `MACOS_NOTARY_TEAM_ID` | the team ID |

`scripts/ci/macos-sign.sh` then signs with the hardened runtime and a secure timestamp, using the
stable identifier `keyboard-chatter-filter`, and submits the binary to Apple's notary service
(`xcrun notarytool submit --wait`). Bare executables cannot be stapled; Gatekeeper retrieves the
ticket online.

Why it matters: with a Developer ID signature, the Accessibility permission survives updates, and
users who download the archive with a browser are not blocked by Gatekeeper. Ad-hoc signatures
change with every build, so users must re-enable Accessibility after each update.

### Windows: Authenticode

| Secret | Value |
|---|---|
| `WINDOWS_CERTIFICATE_PFX_BASE64` | base64 of a code-signing certificate (.pfx) |
| `WINDOWS_CERTIFICATE_PASSWORD` | its password |

The workflow signs with SHA-256 and an RFC 3161 timestamp. `install.ps1` reports the signature and,
with `KCF_REQUIRE_SIGNATURE=1`, refuses unsigned binaries. Cloud signing services (e.g. Azure
Trusted Signing) can replace the PFX step.

## Forks

The installers default to `Shan-e-Shafiq/keyboard-chatter-filter`. Release copies of the
installers are rewritten to the publishing repository automatically. For the `raw.githubusercontent.com`
one-liner from a fork, change `DEFAULT_REPO` in `install.sh` and the default in `install.ps1`, or
set `KCF_REPO`.

## Checklist before announcing a release

* CI and the release workflow are green on all six targets.
* `curl -fsSL …/install.sh | bash` works on macOS (both architectures) and Linux, and
  `install.ps1` on Windows, against the published release.
* The manual checklists in [platform-macos.md](platform-macos.md) and
  [platform-windows.md](platform-windows.md) pass with a physical keyboard.
