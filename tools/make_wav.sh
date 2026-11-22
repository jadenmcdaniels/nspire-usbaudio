#!/bin/sh
# Turn a song you own into a WAV usbaudio.tns can play, named so the
# calculator keeps it (TI's transfer tools only take files ending in .tns).
#
#   tools/make_wav.sh song.mp3                  -> music.wav.tns (22.05 kHz mono 16-bit)
#   tools/make_wav.sh song.flac out.wav.tns 44100 2 60
#        rate (Hz), channels (1 or 2), optional length limit in seconds
#        The full song is kept by default. Existing output is never replaced.
#
# Size: 22.05 kHz mono 16-bit is 44 KB per second (2.6 MB a minute); the
# calculator's storage is the limit. Put the result in /documents/usbaudio/
# on the calculator as music.wav.tns, or use a song name and /music for Music.
set -e
in=${1:?usage: make_wav.sh input [output.wav.tns] [rate] [channels] [seconds]}
out=${2:-music.wav.tns}
rate=${3:-22050}
ch=${4:-1}
secs=${5:-}
command -v ffmpeg >/dev/null || { echo "make_wav.sh: needs ffmpeg" >&2; exit 1; }
[ ! -e "$out" ] || { echo "make_wav.sh: output already exists: $out" >&2; exit 1; }
tmp=$(mktemp "${out}.XXXXXX")
trap 'rm -f "$tmp"' EXIT HUP INT TERM
set --
[ -z "$secs" ] || set -- -t "$secs"
ffmpeg -nostdin -hide_banner -loglevel error -y -i "$in" "$@" -vn -map_metadata -1 \
       -ac "$ch" -ar "$rate" -c:a pcm_s16le -f wav -bitexact "$tmp"
ln "$tmp" "$out"   # same directory/filesystem, atomic and cannot replace an existing song
ls -l "$out"
