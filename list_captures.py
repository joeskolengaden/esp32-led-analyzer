#!/usr/bin/env python3
"""
list_captures.py - retrieval side of host_record.py's guided sessions: scans
captures/*.json and prints a queryable summary instead of you having to open
files by hand and remember what's in each one.

Usage:
    python3 list_captures.py                       # every session, newest first
    python3 list_captures.py --ic TM1814            # only sessions for that IC
    python3 list_captures.py --failed               # only sessions with a
                                                      # colour that didn't fully pass
    python3 list_captures.py --show <session_id>    # full detail for one session
"""
import argparse
import json
from pathlib import Path


# ============================================================================
# Loading + pass/fail evaluation
# ============================================================================

def load_sessions(captures_dir):
    sessions = []
    for jf in sorted(captures_dir.glob("session_*.json"), reverse=True):
        try:
            with open(jf, encoding="utf-8") as f:
                d = json.load(f)
        except (json.JSONDecodeError, OSError):
            continue
        d["_path"] = jf
        sessions.append(d)
    return sessions


def session_passed(session):
    return all(s["frame_count"] > 0 and s["pass_count"] == s["frame_count"] for s in session.get("segments", []))


# ============================================================================
# Display: a compact one-line-per-session table, and a full per-frame detail
# view for a single session (--show)
# ============================================================================

def print_table(sessions):
    if not sessions:
        print("No recorded sessions found.")
        return
    def trunc(s, n):
        return s if len(s) <= n else s[: n - 1] + "…"

    print(f"{'session id':<28} {'IC':<26} {'LEDs':>5} {'mode':<12} {'colours':<8} {'verdict'}")
    print("-" * 100)
    for s in sessions:
        colours = ", ".join(seg["colour"] for seg in s.get("segments", []))
        verdict = "ALL OK" if session_passed(s) else "CHECK"
        print(f"{trunc(s.get('session_id', '?'), 28):<28} {trunc(s.get('ic_name', '?'), 26):<26} "
              f"{s.get('led_count', '?'):>5} {s.get('mode', '?'):<12} "
              f"{len(s.get('segments', [])):<8} {verdict}")
        if colours:
            print(f"    colours: {colours}")


def print_detail(session):
    print(f"session_id : {session.get('session_id')}")
    print(f"ic_name    : {session.get('ic_name')}")
    print(f"mode       : {session.get('mode')}")
    print(f"led_count  : {session.get('led_count')}")
    print(f"notes      : {session.get('notes') or '(none)'}")
    print(f"started    : {session.get('started')}")
    print(f"duration   : {session.get('duration_s'):.1f}s" if session.get("duration_s") is not None else "")
    print(f"expected bytes/frame: {session.get('expected_bytes_per_frame')}")
    print(f"log file   : {session['_path'].with_suffix('.log')}")
    print()
    for seg in session.get("segments", []):
        verdict = "OK" if seg["frame_count"] and seg["pass_count"] == seg["frame_count"] else \
                  ("NO FRAMES" if not seg["frame_count"] else "SOME FAILED")
        rgbw = seg.get("intended_rgbw") or []  # None for the synthetic "(interrupted)" segment
        print(f"  {seg['colour']:<22} R={rgbw[0] if len(rgbw)>0 else '?'} "
              f"G={rgbw[1] if len(rgbw)>1 else '?'} B={rgbw[2] if len(rgbw)>2 else '?'} "
              f"W={rgbw[3] if len(rgbw)>3 else '?'}   {seg['pass_count']}/{seg['frame_count']} frames  {verdict}")
        for f in seg.get("frames", []):
            if f["frame_type"] == "single-wire":
                print(f"      bytes={f.get('bytes_hex', '')}")
                if f.get("timing_matches"):
                    print(f"      timing: {', '.join(f['timing_matches'])}")
                if f.get("signature_matches"):
                    print(f"      signatures: {', '.join(m['name'] for m in f['signature_matches'])}")
            else:
                print(f"      bytes={f.get('bytes_hex', '')}")


# ============================================================================
# CLI entry point: --show for one session's detail, otherwise the filtered
# table (--ic / --failed narrow it down)
# ============================================================================

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-d", "--dir", default="captures", help="captures directory to scan")
    ap.add_argument("--ic", help="only show sessions for this IC name (substring match)")
    ap.add_argument("--failed", action="store_true", help="only show sessions with at least one failing colour")
    ap.add_argument("--show", metavar="SESSION_ID", help="print full detail for one session")
    args = ap.parse_args()

    captures_dir = Path(args.dir)
    if not captures_dir.exists():
        print(f"No such directory: {captures_dir}")
        return

    sessions = load_sessions(captures_dir)

    if args.show:
        matches = [s for s in sessions if args.show in s.get("session_id", "")]
        if not matches:
            print(f"No session matching '{args.show}'.")
            return
        for s in matches:
            print_detail(s)
            print()
        return

    if args.ic:
        sessions = [s for s in sessions if args.ic.lower() in s.get("ic_name", "").lower()]
    if args.failed:
        sessions = [s for s in sessions if not session_passed(s)]

    print_table(sessions)


if __name__ == "__main__":
    main()
