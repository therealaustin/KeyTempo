#!/usr/bin/env python3
"""Builds a kt_tempo_bench manifest from a rekordbox collection export.

Your hand-checked rekordbox beat grids become the answer key: BPM, the time of a
downbeat (beat 1) and the key. Each track is decoded once with ffmpeg to mono
44.1 kHz float32 in OUT_DIR (skipped when the .f32 already exists).

    python3 tests/rekordbox_tracks.py rekordbox.xml OUT_DIR [--playlist "Grid checked"]
    build-core/kt_tempo_bench --files OUT_DIR/tracks.tsv

In rekordbox: File > Export Collection in xml format. Use --playlist with a playlist
that holds only tracks whose grids you have checked by hand; auto-analysed grids are
another analyser's guess, not an answer key.

Tracks whose grid has more than one tempo are left out (the analyser assumes a
constant tempo) and listed at the end. Audio stays outside the repository.
"""

import argparse
import os
import subprocess
import sys
import urllib.parse
import xml.etree.ElementTree as ET

SAMPLE_RATE = 44100


def track_path(location):
    url = urllib.parse.urlparse(location)
    path = urllib.parse.unquote(url.path)
    # Windows exports look like file://localhost/C:/Music/...
    if len(path) > 2 and path[0] == "/" and path[2] == ":":
        path = path[1:]
    return path


def downbeat(track):
    """Seconds of the first downbeat in the grid, or None if the track has no grid."""
    tempos = track.findall("TEMPO")
    if not tempos:
        return None
    t = tempos[0]
    start, bpm = float(t.get("Inizio", "0")), float(t.get("Bpm", "0"))
    beat = int(t.get("Battito", "1"))
    beats_per_bar = int(t.get("Metro", "4/4").split("/")[0]) or 4
    if bpm <= 0:
        return None
    # Battito is the bar position of the beat at Inizio; step forward to the next beat 1.
    return start + ((beats_per_bar - beat + 1) % beats_per_bar) * 60.0 / bpm


def is_variable(track):
    return len({round(float(t.get("Bpm", "0")), 3) for t in track.findall("TEMPO")}) > 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xml")
    ap.add_argument("out_dir")
    ap.add_argument("--playlist", help="only tracks in this playlist (name)")
    args = ap.parse_args()

    root = ET.parse(args.xml).getroot()
    tracks = {t.get("TrackID"): t for t in root.iter("TRACK") if t.get("Location")}

    if args.playlist:
        nodes = [n for n in root.iter("NODE") if n.get("Name") == args.playlist and n.get("Type") == "1"]
        if not nodes:
            sys.exit(f"playlist not found: {args.playlist}")
        ids = [k.get("Key") for k in nodes[0].findall("TRACK")]
    else:
        ids = list(tracks)

    os.makedirs(args.out_dir, exist_ok=True)
    rows, variable, missing = [], [], []
    for track_id in ids:
        t = tracks.get(track_id)
        if t is None:
            continue
        name = f"{t.get('Artist', '')} – {t.get('Name', '')}".strip(" –")
        src = track_path(t.get("Location"))
        if not os.path.exists(src):
            missing.append(name)
            continue
        if is_variable(t):
            variable.append(name)
            continue
        tempos = t.findall("TEMPO")
        bpm = float(tempos[0].get("Bpm")) if tempos else float(t.get("AverageBpm", "0"))
        if bpm <= 0:
            continue

        dst = os.path.join(args.out_dir, f"{track_id}.f32")
        if not os.path.exists(dst):
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-ac", "1", "-ar", str(SAMPLE_RATE), "-f", "f32le", dst],
                           check=True)
        db = downbeat(t)
        rows.append([dst, str(SAMPLE_RATE), f"{bpm:.3f}", name.replace("\t", " "),
                     "" if db is None else f"{db:.4f}", t.get("Tonality", "")])

    manifest = os.path.join(args.out_dir, "tracks.tsv")
    with open(manifest, "w", encoding="utf-8") as f:
        for r in rows:
            f.write("\t".join(r) + "\n")

    print(f"{len(rows)} tracks -> {manifest}")
    if variable:
        print(f"Left out {len(variable)} with a variable-tempo grid: " + "; ".join(variable))
    if missing:
        print(f"Left out {len(missing)} whose file was not found: " + "; ".join(missing))


if __name__ == "__main__":
    main()
