# Queue Page Design

The QUEUE page replaces the standalone Volume page in the main stack. Volume
control stays on the Now Playing page. The live fader and the 5% `-` and `+`
controls give one-gesture volume control there.

The Queue page shows the current item, then a compact list of upcoming items.
The first version of this page gives three functions: `ADD ALBUM`,
`SEARCH SONGS`, and `CLEAR`. Later versions can add row actions: play now,
play next, remove, and drag to reorder. These row actions need a
hardware-verified Music Assistant queue service first.

The `ADD ALBUM` function opens the saved and runtime album list, in an
explicit add-to-queue mode. The `SEARCH SONGS` function opens the existing
keyboard directly and returns Spotify track results. The user can add any
Spotify song to the queue through this function.

The browser keeps its default action: a tap on an album plays the album. The
`ADD ALBUM` function on the Queue page is a separate mode. This mode changes
the tap action: a tap on an album adds the album to the queue.

Music Assistant is the primary queue backend. A Spotify Connect account
target may support playback transfer, but may not support a queue change.
The interface must state this limit to the user. The interface must not
drop an add request with no explanation.
