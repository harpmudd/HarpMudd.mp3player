# v1.8.2 release notes

The GitHub release body, verbatim. House format, the same as every release
since v1.5.0: bold section labels, one flat bullet an item, each opening
with the change itself in bold. `CHANGELOG.md` is the running record; if the
two disagree, the changelog is right.

---

**Fixed**

- **The previous song's lyrics stayed on screen through a track change**, until the new track's artwork had finished loading.
- **Resume came back at the wrong track on a playlist longer than 128 songs** — it returned to one exactly 128 earlier in the list.
- **LOADING TRACK appeared on a blanked screen and stayed there.** A track change is not supposed to wake a sleeping player.
