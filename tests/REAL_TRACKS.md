# Real-track results

Tracks supplied by the project owner as ones that other BPM analyzers get wrong. The audio is
not in the repository. To re-run, decode each file and list it in a manifest:

```bash
ffmpeg -i "song.mp3" -ac 1 -ar 44100 -f f32le song.f32
printf "song.f32\t44100\t174\tsong name\n" >> tracks.tsv
build-core/kt_tempo_bench --files tracks.tsv                 # Auto
build-core/kt_tempo_bench --files tracks.tsv --genre-index 3 # Drum & Bass preset
```

The true BPM was measured by folding each whole track's onset envelope at candidate tempos in
0.005 BPM steps (`precise.py`-style check). For DnB, Beatport lists exactly half of it.

| Track | True BPM | Beatport | v0.2 (Auto) | v0.3 (Auto) | v0.3 locks on after |
|---|---|---|---|---|---|
| Noisia & The Upbeats – Dead Limit | 172 | 86 | 172.0 | 172.0 | 5 s |
| ACP, Jenks (UK) – Rinseout | 174 | 87 | **116.0** (3:2) | 174.0 | 4 s |
| Sub Focus, bbyclose – On & On | 174 | 87 | 174.0 | 174.0 | 22 s ¹ |
| Netsky – I See The Future In Your Eyes | 173 | 87 | 173.0 | 173.0 | 50 s ² |
| Arcando – Ultrasound | 175 | 88 | 175.0 | 175.0 | 5 s |
| Paul Oakenfold – Southern Sun (DJ Tiësto Mix) | 137 | 137 | 137.0 | 137.0 | 6 s |
| P.O.S. – Remember (Summer Sun) | 139.0 | 139 | 139.0 | 139.0 | 4 s |
| Push – Strange World 2000 | 139.2 | 139 | 139.2 | 139.2 | 4 s |
| Mat Zo – The Fractal Universe | 132 | 132 | 132.0 | 132.0 | 4 s |
| Sander Kleinenberg – My Lexicon | 134 | 134 | 134.0 | 134.0 | 5 s |
| **Correct** | | **5 / 10** | **9 / 10** | **10 / 10** | |

¹ The right tempo family from 4 s; the intro only resolves to ~173.7, so it settles on 174.0 at 22 s.
² Half-time intro, then a clap-only section; full-tempo drums start at ~48 s. With the
**Drum & Bass** genre selected it locks at 4 s (every DnB track here locks within 6 s except On & On).

"Locks on" = first time the shown tempo is within ±0.25 BPM; after that it stays correct for
89–100% of the track.
