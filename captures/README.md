# Recorded capture sessions

Written by `../host_record.py`. Two files per **guided** session (the
default mode, `session_<YYYYMMDD_HHMMSS>_<ic>.{log,json}`):
- `.log` — plain text: a header (IC, LED count, start time, port, baud),
  one line per device output with a per-line elapsed-time stamp, and a
  footer (end time, duration, interrupted flag, colours tested, frame
  pass/fail counts).
- `.json` — structured: IC name, mode, LED count, notes, expected
  bytes/frame, and every colour segment's intended RGBW + parsed frames
  with a pass/fail verdict each. This is what `../list_captures.py` reads.
  A session ended early with Ctrl+C still has a `.json`, with
  `interrupted: true` and a final `"(interrupted)"` segment holding
  whatever frames were captured before the interrupt — nothing is lost.

The old **freeform** mode (`--freeform`, no IC/colour structure) writes
just a `.log`, named `session_<YYYYMMDD_HHMMSS>[_<label>].log`.

See "Recording captures for later analysis" in `../README.md` for how to
make one, and `../list_captures.py` for browsing what's here instead of
opening files by hand. Nothing in here is auto-generated on a schedule —
it's exactly the sessions someone deliberately recorded.
