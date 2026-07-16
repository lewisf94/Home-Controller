# Queue Page Design

Replace the standalone Volume page in the main stack with `QUEUE`. Volume
remains on Now Playing, where the live fader and 5% `-` / `+` controls already
make it a one-gesture action.

The Queue page shows the current item followed by a compact upcoming list.
The first implementation provides `ADD ALBUM`, `SEARCH SONGS`, and `CLEAR`.
Play-now, play-next, remove, and drag-reorder are follow-on row actions once
the basic Music Assistant queue service path has been hardware-verified.

`ADD ALBUM` opens the saved/runtime album selection in an explicit add-to-queue
state. `SEARCH SONGS` opens the existing debounced keyboard directly and returns
Spotify track results, so the user can queue any Spotify song.

Browsing keeps its safe default: tapping an album plays it. The Queue page's
`ADD ALBUM` entry is the deliberate mode switch that makes a selected album
append to the queue instead.

Music Assistant is the primary queue backend. Spotify Connect account targets
may support playback transfer but not queue mutation, so the UI must explain
that distinction rather than silently dropping an add request.
