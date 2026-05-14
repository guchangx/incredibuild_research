# Notepad ProcMon Baseline Test

This folder contains a safe baseline test for observing normal Notepad file I/O.

It does not hide, patch, inject into, or tamper with Notepad. The goal is to
produce a repeatable reference case that Process Monitor, ETW, or Sysmon should
be able to observe.

## Files

- `run_notepad_baseline.ps1` starts Notepad with a known target file path.
- `baseline_output.txt` is created by the script as the save target.

## Manual test

1. Start Process Monitor as Administrator.
2. Add filters:
   - `Process Name` `is` `notepad.exe` then `Include`
   - `Path` `contains` `baseline_output.txt` then `Include`
3. Run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\run_notepad_baseline.ps1
   ```

4. In Notepad, type:

   ```text
   hello word
   ```

5. Save the file and close Notepad.
6. Process Monitor should show normal file activity for `notepad.exe`, including
   create/write/cleanup operations against `baseline_output.txt`.

## Notes

On recent Windows versions Notepad may run as a packaged app or use a brokered
save flow depending on how it is launched and how the file picker is used. This
script launches Notepad with a concrete file path to keep the operation easy to
filter and compare.
