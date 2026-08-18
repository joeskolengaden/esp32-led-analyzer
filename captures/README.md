# Recorded capture sessions

Written by `../host_record.py`. Each file is
`session_<YYYYMMDD_HHMMSS>[_<label>].log`: plain text, a header (start time,
port, baud, label), one line per device output with a per-line elapsed-time
stamp, and a footer (end time, duration, line count).

See "Recording captures for later analysis" in `../README.md` for how to make
one. Nothing in here is auto-generated on a schedule — it's exactly the
sessions someone deliberately recorded.
