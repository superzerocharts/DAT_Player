# Spotter Player File Access Notes

Sample folder:

`C:\Users\schmijon\Projects\Camera 205 Sample\Camera 205 Sample`

## GUI observation

I launched:

```powershell
SpotterPlayer.exe "C:\Users\schmijon\Projects\Camera 205 Sample\Camera 205 Sample\Camera 205 Sample.sef2"
```

Result:

- `SpotterPlayer.exe` started as a launcher process.
- It spawned the real UI process:
  - `C:\Users\schmijon\AppData\Roaming\DVMS\spotter\9.9.5\Offline\Spotter.exe`
- The visible window title was `Spotter Player - 9.9.5.1`.
- UI Automation exposed:
  - Date/time picker value near clip start: `5/19/2026 9:25:17 PM`
  - Video overlay: `9:25:17 PM  5/19/2026`
  - Camera name: `205 Choke Point`
- A screenshot was captured at `docs/spotter_player_start_display.png`; because playback was running, the screenshot shows a slightly later overlay value, `9:26:16 PM  5/19/2026`.
- Sysinternals Process Monitor was not available on `PATH`, so I did not capture a file I/O trace.

## File access conclusion

Direct file access order was not captured in this pass. Based on the file contents, Spotter can obtain the recording time from at least the `.sef2` file alone, because it contains explicit XML `<start>` and `<end>` values plus serialized timezone data. The same raw start/end values are also duplicated in `MaterialFolderIndex.dat` and in `dvrfile00000001.dat`.

Recommended optional follow-up:

- Run Process Monitor manually with filters:
  - Process Name is `SpotterPlayer.exe`
  - Path contains `C:\Users\schmijon\Projects\Camera 205 Sample\Camera 205 Sample`
- Open `Camera 205 Sample.sef2`.
- Save the event list or record whether Spotter reads `.sef2`, `MaterialFolderIndex.dat`, and `dvrfile00000001.dat`, and in which order.
