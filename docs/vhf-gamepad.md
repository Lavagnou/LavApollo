# VHF virtual gamepad

LavApollo can emulate gamepads through a second driver besides ViGEmBus: the VHF virtual gamepad
driver from [libvirtualgamepad](https://github.com/Nonary/libvirtualgamepad) (Nonary, MIT), the one
Vibepollo ships. It presents an Xbox Series, Xbox One, DualShock 4, DualSense or Switch Pro
controller, and the PlayStation profiles carry the touchpad, motion sensors, battery and lightbar.

## Choosing it

In *Configuration → Input → Emulated Gamepad Type*:

| Value | Behaviour |
|---|---|
| `auto` | ViGEmBus when it is installed; the VHF driver when it is not. |
| `vhf` | The VHF driver, following the client's controller: DualSense for a PlayStation pad (or when motion or touchpad emulation applies), Switch Pro for a Nintendo pad, Xbox otherwise. |
| `vhf_xbox`, `vhf_xbox_one`, `vhf_ds4`, `vhf_ds5`, `vhf_switch` | The VHF driver, always as that controller. |

Each gamepad slot remembers which driver created it, so both drivers can serve gamepads in the same
session. If the VHF driver cannot create a controller, LavApollo falls back to ViGEmBus for that
slot.

## Signing

The libvirtualgamepad release archive is **unsigned**. Windows only installs a driver package whose
catalogue is signed by a publisher it trusts, so the release workflow signs it with a LavApollo
certificate before bundling it:

- `driver/VibeshineVhfGamepad.cat` and `tools/VibeshineVhfGamepadDeviceSetup.exe` are signed;
- `driver/VibeshineVhfGamepad.dll` is **not**: the catalogue hashes it, and signing it would break
  the catalogue;
- the public certificate ships as `driver/LavApolloVhfGamepad.cer`. At install time, `install.ps1`
  checks that this certificate signed both files, then adds it to `LocalMachine\Root` and
  `LocalMachine\TrustedPublisher`. That is the trust model SudoVDA already uses with `sudovda.cer`.

The archive itself is pinned by release URL and SHA-256 in `scripts/prepare-vhf-gamepad.ps1`. The
client protocol compiled into LavApollo comes from the `third-party/libvirtualgamepad` submodule,
pinned to that release's source commit. **Move both together.**

### One-time setup

1. On a machine you trust, run:
   ```powershell
   .\scripts\generate-vhf-signing-cert.ps1
   ```
   It writes a self-signed code-signing certificate (10 years) to `vhf-signing\`.
2. In the repository's *Settings → Secrets and variables → Actions*, create two secrets:
   - `VHF_SIGNING_CERT_BASE64`, set to the contents of `vhf-signing.pfx.b64`;
   - `VHF_SIGNING_CERT_PASSWORD`, set to the contents of `vhf-signing.password.txt`.
3. Delete `vhf-signing\`, or move it offline.

Without these secrets, the release still builds. It ships without the VHF driver, and the workflow
prints a warning.

### The risk

Every PC that installs LavApollo with this driver trusts the certificate as a root and as a trusted
publisher. Whoever holds the private key can sign software that these PCs will trust. The key must
live only in the GitHub secrets. Uninstalling the driver (the uninstaller asks) removes the
certificate from both stores.

## Local builds

Point CMake at a prepared package:

```powershell
.\scripts\prepare-vhf-gamepad.ps1 -OutDir build\vhf-gamepad -PfxPath <pfx> -PfxPassword <password>
cmake -B build -G Ninja -S . -DLAVAPOLLO_VHF_GAMEPAD_PACKAGE_DIR=build/vhf-gamepad
```

The archive is x64-only, like LavApollo's Windows build.
