# KinMap

KinMap is a Qt 6 family relationship viewer for `family_tree_26-27.json`. It fetches the current JSON through `/opt/kinmap/fetch_family_tree.sh`, renders a connected pedigree graph, and supports manual card placement, adaptive card sizing, palettes, and copyable layout metadata.

## v10 workspace

- Two synchronized panes show the same editable tree. Dragging a card updates both panes because both views share one scene.
- Each pane pans and zooms independently. Click a pane and use Ctrl/Alt + or -; pane-specific +/- buttons and Fit L / Fit R are also available.
- Scale/Layout includes global card/spacing controls plus width and height controls for every detected generation.
- Copy Layout exports every card's x/y/width/height/generation, palette, global scale settings, per-generation dimensions, splitter widths, and each pane's zoom, center, and fit state. This is intended to be pasted back into ChatGPT so those values can be made defaults in a later version.

Portable provenance identifier: `AKA_TE324543`.


## KinMap v12 layout defaults

KinMap v12 bundles the latest user-supplied copied layout as `default_layout.json`. The installer places it in the user's application-data directory so it becomes the startup layout. The Scale/Layout table now has a per-generation **Spacing** control in addition to Width and Height. Generation 6 defaults to 300% spacing, while the other generations retain 100% unless changed. Copy Layout records these spacing values, both pane zoom/center/fit states, splitter size, colors, and every node coordinate/dimension.


The v12 bundled layout captures generation widths of 250%, generation 6 spacing of 300%, adaptive fill with a 36 pt maximum font and 40-unit margin, plus the copied two-pane fit/zoom/center state.


KinMap v13 adds a visible sun (☀) update marker at the upper-left of the GUI and a visible v13 title marker so the installed GUI version is immediately recognizable.


## v16 startup defaults

KinMap v16 makes the requested visual defaults explicit in both the bundled layout and the application code: the scene background is pure white (`#ffffff`), Base card + text scale defaults to 250%, and generations 0 through 6 default to 250% card width. The Palette reset also returns to white, and the Scale/Layout reset returns generation widths to 250%. The sun marker and visible `v16` title remain the GUI update check.


## v16 scale correction

The Base card + text scale is now hard-defaulted to 250% in source, the bundled default layout, startup loading, and Reset all defaults. This prevents it from snapping back to 100%.


KinMap v16 uses a palm-tree update marker. The left pane is the navigation master: panning it centers the right pane on the same scene coordinates. When the left pane zoom changes, the right pane is set to 2.5 times the left zoom (for example 8% left = 20% right).


## v17 position calibration

KinMap v17 simplifies the toolbar. Fit L, Fit R, Fit Both, Palette, Scale/Layout, and Copy Layout are removed from the GUI. Reset returns both panes to the same tree center with 13% zoom on the left and 32% on the right. The left pane remains the navigation master, and left zoom keeps the right pane at the 13:32 magnification ratio. Copy Position records both panes' current centers, zooms, visible scene rectangles, viewport dimensions, and splitter widths. Copy Position Log copies the accumulated calibration log as JSON.


## v18
- Added a **Center Me** button that centers the left pane on the Me node and makes the right pane follow to the same scene coordinates.
- Expanded the scene rect with large off-tree margins so you can pan beyond the outermost boxes and center edge nodes like Me or generation-6 ancestors.


## v19 aiming and fast pan
- Added a fixed crosshair across the full left viewport. The intersection marks the exact scene coordinate that the right pane follows.
- Hold **Shift** while left-dragging to pan at 3× normal speed. Release Shift to return to ordinary panning; there is no sticky toggle mode.


## v20
- Crosshair overlay is re-anchored to the current left viewport rectangle on resize, show, move, scroll, and fast-pan updates.
- The reticle therefore remains at the exact visual center of the left pane while the tree moves underneath it.


## v21 persistent family editing
- Select any person card and use **Add Parent** or **Add Child**.
- Existing names are linked; new names create local person records.
- Manual card positions, local people, and local parent/child relationships are saved to `/var/lib/kinmap/kinmap_state.json`.
- The Pi JSON remains the upstream source; local edits are overlaid after each reload.
- `/var/lib/kinmap` is deliberately outside `/tmp` and `/home/we6jbo/`.


## v22
- New local parent/child cards are placed near the selected person inside the current left-pane view.
- Pi JSON people are read-only for edit/delete operations.
- Locally-added people use a green-blue border and can be edited or deleted.
- Local people can store birth date/location and death date/location.
- Each local person gets a monotonic permanent `TG09xxxxxx` identifier; issued identifiers remain retired after deletion.
- Positions, local people, relationships, and TG09 allocation state persist in `/var/lib/kinmap/kinmap_state.json`.


## v23 recording workflow
- **Record** toggles to **Stop Recording** and captures left/right viewport research snapshots while the user navigates.
- Snapshots are written beneath `/home/we6jbo/familyhistory/here_YYYYMMDD_HHMMSS/`.
- Only Pi people that have real TG identifiers and locally-added green-blue people are included in the recording.
- Each snapshot stores visible people, their visible relationships, and left/right viewport coordinates.
- Stopping recording writes `session_summary.json`, `vibe_prompt.txt`, and an executable `/tmp/vibesep2726/here_YYYYMMDD_HHMMSS.sh`.
- Running that launcher starts Mistral Vibe from `/home/we6jbo/familyhistory/` with the recorded session as research context and privacy-aware Moltbook follow-up instructions.


## v24
Newly-created parents and children are inserted directly beside the selected person in the current left-pane area. Adding a relationship no longer triggers a full automatic tree rebuild, preventing local relatives from being sent to the detached far-right region. The nearby position is persisted immediately in `/var/lib/kinmap/kinmap_state.json`.
