# Spotter Tamper Verification Audit

## Summary

SpotterPlayer/Spotter does expose and perform an authenticity check for protected media formats. The installed Spotter UI resources contain these status strings:

- `Media file authenticity verified`
- `Media file authenticity could not be verified`
- `The media file is not authentic`
- `Verifying media file authenticity`

The live UI also exposes a `_mediaFileStatus` control, and the Spotter executable contains related members such as `_authenticityProgressBar`, `_panelAuthenticity`, `_imageAuthentic`, `_imageNotAuthentic`, `AuthenticityStatusEvent`, and `AuthenticityProgressEvent`.

Follow-up verification confirmed that `FileStorage.dll` embeds `Mirasys.FileStorage.SigningPublicKey.xml`, and .NET `SignedXml` can verify the original `Camera 205 Sample.sef2` with that public key. The original SEF2 passes, a copy with the start timestamp changed fails, and a copy with the `<Signature>` element removed has no signature to verify.

Static inspection shows the verification stack is deeper than a simple file-size check. `FileStorage.dll` contains methods/strings for `VerifyXmlFile`, `ComputeSignature`, `CheckSignature`, `StartIntegrityCheck`, `IntegrityCheckTask`, and watermark validation errors such as `Frame watermark is invalid!`. The `.sef2` contains a W3C XML digital signature over the sidecar metadata, while the video/index data appears to use proprietary material/frame watermark structures.

DAT Player can likely implement partial checks now, but should not claim official Spotter-equivalent verification until the XML signing public key and frame/material watermark algorithm are fully understood and matched.

## Test Inputs

Executable:

- `C:\Users\schmijon\Projects\Camera 205 Sample\Camera 205 Sample\SpotterPlayer.exe`
- SHA-256: `4A6E62A4F9D6BEB0FCC9B69EE1F80EDE703DF44BDDF9674D8BCFEA433CF48638`

Known-good export folder:

- `C:\Users\schmijon\Projects\Camera 205 Sample\Camera 205 Sample`

Known-good files:

| File | Size | SHA-256 |
| --- | ---: | --- |
| `Camera 205 Sample.sef2` | 3,786 | `3D6F9F19E7D96914CA9CE650B9AF328E1B39337EC96555D429F503C12AE6F5DF` |
| `dvrfile00000001.dat` | 194,760,704 | `FF8A8DA4FC31990A2AEAAAE068309569E5DF740CAE2A21F11C7BFFCB9F62F8BC` |
| `MaterialFolderIndex.dat` | 81 | `58EA69BC4727FA35EE17EC688BD7D7D5A10ABA1EFDDFE7E6A3F6A2CE22B50412` |

Additional DAT-only folder:

- `C:\Users\schmijon\Projects\Camera 205 Sample`
- Contains raw `.dat` copies without the `.sef2` in the same folder. This is useful as a negative/control case: without SEF/SEF2/ASF/VAF container metadata, Spotter’s official protected-media authenticity path is not expected to apply.

Tamper-test copies were created under:

- `C:\Users\schmijon\Projects\DAT_Player\research_tmp\spotter_tamper_tests`

Original evidence files were not modified.

## Verification Entry Points Checked

Command line:

- Tried `/?`, `--help`, `-help`, `/help`, and `--version`.
- The launcher stayed resident and did not print a useful help/version response.
- Opening a `.sef2` as a command-line argument works and logs `WindowMain.OpenClip`.

UI:

- `File > Open Media` / `F4` opens a Windows file dialog filtered to protected media-like formats:
  - `*.vaf`, `*.wmv`, `*.wma`, `*.asf`, `*.sef`, `*.sef2`, `*.esef`
- The UI exposes a `_mediaFileStatus` control.
- Resource strings and UI member names indicate automatic authenticity status display rather than a separate manual verification command.
- Ribbon tabs observed: File, Search, Devices, View, Help. No standalone “Verify” button was exposed in UI Automation.

Logs:

- Spotter logs are written under:
  - `C:\Users\schmijon\AppData\Local\DVMS Client\Sandbox\Spotter Player\9.9.5.130\roaming\modified\@APPDATA@\DVMS\DVR Application\Logs`
- Logs record media open attempts, disk open status, missing DAT file failures, and invalid/tampered `.sef2` failures.
- I did not find log lines spelling out the final UI text `Media file authenticity verified` or `The media file is not authentic`.

Runtime files/modules:

- SpotterPlayer launches a child UI process:
  - `C:\Users\schmijon\AppData\Roaming\DVMS\spotter\9.9.5\Offline\Spotter.exe`
- The sandboxed/extracted bundle contains relevant assemblies:
  - `Spotter.exe`
  - `ArchiveReader.dll`
  - `FileStorage.dll`
  - `FileStorageNative.dll`
  - `ApplicationServices.dll`
  - `ApplicationServicesAPI.dll`
  - `BackupCommunication.dll`

Static strings/method names:

- `Spotter.exe`: `MediaFileAuthentic`, `MediaFileAuthenticityUnknown`, `MediaFileNotAuthentic`, `MediaClipVerifyingAuthenticity`, `AuthenticityStatusEvent`, `AuthenticityProgressEvent`, `_authenticityProgressBar`, `_panelAuthenticity`, `_imageAuthentic`, `_imageNotAuthentic`
- `FileStorage.dll`: `VerifyXmlFile`, `ComputeSignature`, `CheckSignature`, `StartIntegrityCheck`, `IntegrityCheckTask`, `CalculateChecksum`, `XmlDsigEnvelopedSignatureTransform`, `SignedXml`
- `ArchiveReader.dll`: `AuthenticityCheckProgress`, `AuthenticityCheckStatus`, `CheckAuthentication`, `GetDataWatermark`, `ImageWatermark`, `AudioWatermark`, `GetHeaderWatermark`

Embedded FileStorage resources:

- `Mirasys.FileStorage.SigningPublicKey.xml`
- `Mirasys.FileStorage.SigningPrivateKey.xml`

Only the public-key resource was used for verification in this audit.
- `FileStorageNative.dll`: `CalculateHash`

## Observed SpotterPlayer Behavior

Known-good `.sef2` opens and plays normally. The visible overlay shows:

- `205 Choke Point`
- local/Pacific archive time, e.g. `9:25:17 PM  5/19/2026`

When opened through `F4` / Open Media, the file dialog filter confirms Spotter treats SEF/SEF2 as protected media formats. The documentation-provided status strings match the embedded UI resources, with one wording difference:

- Documentation wording: `Media file authenticity is verified`
- Embedded resource wording in this Spotter build: `Media file authenticity verified`

I did not successfully capture the right-side/popup authenticity status panel text through UI Automation during this pass. The underlying UI resources and controls are present, so the failure appears to be observation/automation coverage rather than absence of the feature.

## Tamper Test Results

| Test | File changed | Change made | SpotterPlayer result | Playback result | Notes |
| --- | --- | --- | --- | --- | --- |
| Known-good copy | none | Copied `.sef2`, `.dat`, and `MaterialFolderIndex.dat` | Opened normally | Plays | Used as baseline copy; original not modified |
| DAT payload byte flip | copied `dvrfile00000001.dat` | Flipped one byte at offset `1069200`, inside first-frame payload area | Opened normally through command-line open; authenticity status text not captured | Playback UI loaded | This should be the best case to provoke `The media file is not authentic`; the right-side status text was not captured in automation |
| SEF2 start changed | copied `Camera 205 Sample.sef2` | Changed XML start timestamp by one second, leaving old signature | `SignedXml.CheckSignature(publicKey)` failed; Spotter did not load archive content; log: `Check archive password failed: Archive file reading failed` | No normal playback observed | Confirms `.sef2` metadata signature detects edit |
| SEF2 signature removed | copied `Camera 205 Sample.sef2` | Removed `<Signature>` element | No XMLDSIG signature element; Spotter did not load archive content; log: `Check archive password failed: Archive file reading failed` | No normal playback observed | Indicates signed metadata is required for this export |
| DAT missing | copied folder | Removed copied `dvrfile00000001.dat` | Spotter opened shell UI but logged missing DAT: `Couldn't load file ... dvrfile00000001.dat!` | No video payload available | Confirms `.sef2` references the DAT filename and requires it for playback |
| DAT-only folder | `C:\Users\schmijon\Projects\Camera 205 Sample` | No `.sef2` sidecar in same folder | Not a protected-media authenticity case for Open Media | DAT may be usable by DAT Player, but not official Spotter SEF/SEF2 authenticity | Useful as DAT Player boundary: raw DAT alone should show `Integrity: not checked` |

## Files Used By Verification

Based on metadata and observed behavior:

- `.sef2` is required for official protected-media opening and contains signed metadata.
- `dvrfile00000001.dat` is required for video payload and likely frame/material watermark verification.
- `MaterialFolderIndex.dat` contains start/end tick metadata and likely contributes to archive indexing, but the exact authenticity role remains unproven.
- Spotter logs show the folder is opened as a `Material backup` disk and that missing DAT files are reported.

Relevant log excerpts:

```text
WindowMain.OpenClip - Opening media given from command line argument: ...\Camera 205 Sample.sef2
DiskManager.OpenDisk [Material backup] - Open disk completed, path: ...\Camera 205 Sample\
FileSystemCore.PrintOpenInfo [Material backup] - // Failed disks count: 0
ArchiveHandler.OnCommunication_CheckPasswordEvent [Material backup] - Check archive password failed: Archive file reading failed
<LoadFiles>b__0 [Material backup] - Coudn't load file ...\dvrfile00000001.dat!
```

## Metadata / Signature Findings

`Camera 205 Sample.sef2` is plain XML and includes:

```xml
<files>
  <file name="dvrfile00000001.dat" hash="194760705" />
</files>
```

The `hash` value is not a SHA-256/MD5-style digest. For this sample it equals the DAT size plus one:

- DAT size: `194760704`
- XML `hash`: `194760705`

The `.sef2` also contains a W3C XML digital signature:

```xml
<Signature xmlns="http://www.w3.org/2000/09/xmldsig#">
  <SignedInfo>
    <CanonicalizationMethod Algorithm="http://www.w3.org/TR/2001/REC-xml-c14n-20010315" />
    <SignatureMethod Algorithm="http://www.w3.org/2001/04/xmldsig-more#rsa-sha256" />
    <Reference URI="">
      <Transforms>
        <Transform Algorithm="http://www.w3.org/2000/09/xmldsig#enveloped-signature" />
      </Transforms>
      <DigestMethod Algorithm="http://www.w3.org/2001/04/xmlenc#sha256" />
      <DigestValue>2rWYkrLPiXSXpu0yv2RXYHc7ah7fzuodOVbYqVObhOw=</DigestValue>
    </Reference>
  </SignedInfo>
  <SignatureValue>...</SignatureValue>
</Signature>
```

Important observations:

- Editing signed `.sef2` metadata caused Spotter to reject/open no archive content.
- Removing the `<Signature>` caused Spotter to reject/open no archive content.
- `FileStorage.dll` references `Mirasys.FileStorage.SigningPublicKey.xml` and `Mirasys.FileStorage.SigningPrivateKey.xml` in strings, but no standalone key XML file was found in the sandbox directory listing.
- `FileStorage.dll` references .NET `SignedXml` and `XmlDsigEnvelopedSignatureTransform`.
- `ArchiveReader.dll` and `FileStorage.dll` contain watermark-related strings, including:
  - `Frame watermark is invalid!`
  - `The 'MaterialFileIndexChunk' watermark is invalid!`
  - `GetDataWatermark`
  - `ImageWatermark`
  - `AudioWatermark`
  - `GetHeaderWatermark`

This suggests two layers:

1. XML signature/authentication for the SEF2 sidecar metadata.
2. Proprietary archive/frame/material watermark checks for video/index data.

### SEF2 XML Signature Verification Result

Using `System.Security.Cryptography.Xml.SignedXml` with the public key extracted from `FileStorage.dll` resource `Mirasys.FileStorage.SigningPublicKey.xml`:

| Case | Signature present | SignedXml result |
| --- | --- | --- |
| Original `Camera 205 Sample.sef2` | yes | pass |
| Baseline copy | yes | pass |
| Start timestamp changed | yes | fail |
| Signature removed | no | not verifiable |

Implementation note: the verification passes when `XmlDocument.PreserveWhitespace` is `false`. Loading the document with whitespace preservation enabled caused `CheckSignature` to return false for the original file.

## Can DAT Player Implement This?

Partial verification appears feasible.

Feasible now:

- Detect whether a `.sef2` sidecar exists.
- Parse `.sef2` and confirm it references the expected DAT filename.
- Check the DAT file exists.
- Check DAT size against known sidecar/index expectations where fields are understood.
- Verify the `.sef2` XML digital signature using the embedded Mirasys public key from `FileStorage.dll`.
- Warn when `.sef2` is missing, unsigned, or has an invalid XML signature.
- Report `Integrity: not checked` for raw DAT-only playback.

Current DAT Player implementation:

- Supports conservative SEF/SEF2 XML signature verification for sidecar metadata.
- Uses only the embedded public key extracted from `FileStorage.dll` resource `Mirasys.FileStorage.SigningPublicKey.xml`.
- Does not include, extract, or use the private key resource.
- Verifies with .NET `SignedXml` using `XmlDocument.PreserveWhitespace = false`, matching the observed passing path for the known-good sample.
- Reports sidecar metadata signature status separately from video authenticity.

Not proven yet:

- Full Spotter-equivalent DAT frame/material watermark verification.
- Exact algorithm behind `Frame watermark is invalid!` and `MaterialFileIndexChunk watermark is invalid!`.
- Whether a one-byte H.264 payload change always changes the authenticity status, where the UI exposes that status, and whether playback continues after a failed check.

Not appropriate:

- Forging signatures.
- Patching or bypassing Spotter verification.
- Claiming official Mirasys authenticity unless DAT Player reproduces Spotter behavior exactly against multiple known-good and known-tampered samples.

## Recommended DAT Player Integration

Smallest safe feature:

- Show `Integrity: not checked` for raw `.dat` files opened without `.sef2`.
- If a same-folder `.sef2` exists, show `Integrity metadata: present`.
- Parse `.sef2` and show:
  - referenced DAT filename
  - XML signature present/missing
  - signature method/digest method
  - camera name
  - recording start/end
- Perform conservative consistency checks:
  - referenced DAT exists
  - DAT filename matches sidecar
  - DAT size is plausible against sidecar/index fields
  - first/last DAT frame timestamps match sidecar start/end where applicable
- Use warning language:
  - `Official Spotter authenticity: not verified`
  - `Sidecar consistency: passed/failed`
  - `SEF2 signature: present but not verified`

Later, if public-key XML signature verification is implemented:

- `SEF2 signature: valid/invalid`
- Still avoid `Media file authenticity verified` unless frame/material watermark verification is also proven.

## Risks / Unknowns

- The exact public key/trust root for `.sef2` XML signature verification was not extracted.
- The DAT/frame watermark algorithm is proprietary and not yet understood.
- UI Automation did not capture the right-side/popup authenticity status text, even though the resource strings and controls exist.
- The DAT byte-flip test opened, but the final authenticity status was not captured.
- Some status may appear only after playback advances far enough for an asynchronous `AuthenticityCheckProgress`/`AuthenticityCheckStatus` event.
- The exact role of `MaterialFolderIndex.dat` in authenticity checking is unknown.
- The `.sef2` `hash` attribute is not yet understood; it is not a cryptographic digest in this sample.

## Appendix

Relevant Spotter UI resource strings from `Spotter.exe`:

```text
MediaFileAuthentic = Media file authenticity verified
MediaFileAuthenticityUnknown = Media file authenticity could not be verified
MediaFileNotAuthentic = The media file is not authentic
MediaClipVerifyingAuthenticity = Verifying media file authenticity
ErrorNotAuthentic = THE DATA IS NOT AUTHENTIC
```

Relevant static strings/methods:

```text
FileStorage.dll:
  VerifyXmlFile
  ComputeSignature
  CheckSignature
  StartIntegrityCheck
  IntegrityCheckTask
  SignedXml
  XmlDsigEnvelopedSignatureTransform
  Mirasys.FileStorage.SigningPublicKey.xml
  Mirasys.FileStorage.SigningPrivateKey.xml
  Frame watermark is invalid!
  The 'MaterialFileIndexChunk' watermark is invalid!

ArchiveReader.dll:
  AuthenticityCheckProgress
  AuthenticityCheckStatus
  CheckAuthentication
  GetDataWatermark
  ImageWatermark
  AudioWatermark
  GetHeaderWatermark

Spotter.exe:
  _mediaFileStatus
  _authenticityProgressBar
  _panelAuthenticity
  _imageAuthentic
  _imageNotAuthentic
  AuthenticityStatusEvent
  AuthenticityProgressEvent
```

Open Media filter:

```text
Media files (* .vaf; *. Wmv; *. Wma; *. Asf; *. Sef; *. Sef2; *. Esef)
```

Controlled tamper copy paths:

```text
C:\Users\schmijon\Projects\DAT_Player\research_tmp\spotter_tamper_tests\baseline_copy
C:\Users\schmijon\Projects\DAT_Player\research_tmp\spotter_tamper_tests\dat_payload_byte_flip
C:\Users\schmijon\Projects\DAT_Player\research_tmp\spotter_tamper_tests\sef2_start_changed
C:\Users\schmijon\Projects\DAT_Player\research_tmp\spotter_tamper_tests\sef2_signature_removed
C:\Users\schmijon\Projects\DAT_Player\research_tmp\spotter_tamper_tests\dat_missing
```
