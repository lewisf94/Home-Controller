# KiCad Guide: Building the Haptic-Knob Daughterboard

This guide assumes no memory of KiCad, and no prior electronic-design
experience. This guide starts from installing the software, and ends with a
manufacturable board. Read this guide in order, the first time through.

This guide targets KiCad 10, since your project already uses that version.

Read [DESIGN_NOTES.md](DESIGN_NOTES.md) for the hardware decisions this guide
assumes. Read [KNOB-PARTS.md](KNOB-PARTS.md) for the full parts list.

---

## 1. Install KiCad

1. Go to `kicad.org`, and download the installer for your operating system.
2. Install version 10. An older version may open this project with a
   warning, or may not open it at all.
3. Launch KiCad once, after installation. KiCad opens its **KiCad Project
   Manager** window first. This window lists your recent projects, and lets
   you create or open a project. Leave this window open; the next section
   uses it.

---

## 2. What each KiCad file and window actually is

A KiCad project uses several files, all sharing one base name. The base name
of your project is `home-controller-daughterboard`:

- `home-controller-daughterboard.kicad_pro`: the project file. Double-click
  this file, or select it from the Project Manager, to open the project.
- `home-controller-daughterboard.kicad_sch`: the schematic. This file records
  which parts exist, and how their pins connect to each other, as a diagram.
- `home-controller-daughterboard.kicad_pcb`: the PCB layout. This file
  records the physical copper board: part positions, copper traces, and
  drill holes.
- `home-controller-daughterboard.kicad_prl`: per-person view settings, for
  example which layers are visible. This file has no effect on the actual
  design.
- `sym-lib-table` and `fp-lib-table`: two small files that tell this specific
  project where to find a custom part. Section 5 explains these two files in
  full.

Two separate editor windows work on these files:

- **Schematic Editor**. Opens the `.kicad_sch` file. You draw your circuit
  diagram here first.
- **PCB Editor**. Opens the `.kicad_pcb` file. You lay out the physical board
  here, after the schematic is complete.

Two more terms matter throughout this guide:

- A **symbol** is the diagram drawing of a part, in the Schematic Editor: a
  rectangle with labelled pins.
- A **footprint** is the physical pad pattern for that same part, in the PCB
  Editor: the actual copper pads a real chip solders onto.

Every symbol links to exactly one footprint. KiCad keeps the two separate on
purpose. One symbol, for example a generic resistor, can use many different
footprints, for many different physical part sizes.

---

## 3. Where this project lives, and why

This project already exists in this repository, at this exact path:

```
pcb/home-controller-daughterboard/
```

This is the correct location for it, and the reason matters for how you work
from here on. A fresh copy of this repository needs every file that opens
and builds this board correctly. Each such file lives inside the repository
itself, checked in with `git`, not only saved on your own computer. This
single rule answers both of your questions at once: where to save your
work, and how another reader, including a future session with me, can check
it. I read the plain text of every file already committed to this
repository. A file that exists only on your own computer, and was never
committed, stays invisible to me. This same file stays invisible to any
other computer that clones this repository fresh.

The `pcb/home-controller-daughterboard/` folder currently holds:

- The four project files from Section 2.
- `sym-lib-table` and `fp-lib-table`, added in this session. Section 5
  explains why these two files were missing, and what they now fix.

One more folder matters to this same project, even though it sits in a
different part of the repository:

```
docs/Symbols & Footprints/TMC6300-LA/
```

This folder holds a custom part: the exact TMC6300-LA symbol, footprint,
and 3D model that the `U1` component in your schematic already uses. This
part came from a component search site named SnapEDA. This folder was
already committed to the repository, but the new `sym-lib-table` and
`fp-lib-table` files, from
Section 5, are what actually connect your project to it.

---

## 4. Opening the project for the first time

1. Open the KiCad Project Manager.
2. Select **File → Open Project**.
3. Navigate to `pcb/home-controller-daughterboard/`, inside your local copy
   of this repository.
4. Select `home-controller-daughterboard.kicad_pro`, then select **Open**.
5. The Project Manager now shows this project, with the Schematic Editor and
   the PCB Editor available as buttons.
6. Select **Schematic Editor**, to open your existing circuit diagram.

---

## 5. The library gap this session closed

Before this session, the `U1` symbol in your project, the TMC6300-LA,
worked on your own computer. This symbol likely would not have opened
correctly on a different computer, including a fresh clone of this
repository. The reason: KiCad finds a custom symbol through a library
table, and your project had no project-specific table of its own. Your copy
of KiCad most likely found this part a different way instead. This other
way was a library added to the global settings of your own computer,
outside this repository. This part therefore stayed invisible to anyone
else.

This session added the two missing files, directly inside
`pcb/home-controller-daughterboard/`:

- `sym-lib-table` names one library, `TMC6300-LA`, and points it at
  `docs/Symbols & Footprints/TMC6300-LA/TMC6300-LA.kicad_sym`, using a path
  relative to the project folder.
- `fp-lib-table` names the same library, and points it at the containing
  folder, `docs/Symbols & Footprints/TMC6300-LA/`, for the matching
  footprint.

Both files use a KiCad path variable, `${KIPRJMOD}`. This variable always
stands for the folder the project file sits in. A relative path built from
this variable keeps working after a fresh clone. This same path keeps
working on any computer, and in any folder location. This variable never
names the specific folder structure of your own computer.

Every other symbol already on your sheet needs no such file at all. The
RP2040, the HX711, and the MT6701QT each come from a library included with
every KiCad installation, on every computer, by default. Only a custom
part, sourced from outside KiCad, needs a project-specific entry like this
one.

---

## 6. Confirming the fix

Do this once, the first time you open the project after this change:

1. In the Schematic Editor, open **Preferences → Manage Symbol Libraries**.
2. Select the **Project Specific Libraries** tab.
3. Confirm a row named `TMC6300-LA` already appears, pointing at a path
   ending in `docs/Symbols & Footprints/TMC6300-LA/TMC6300-LA.kicad_sym`.
4. Select **Cancel**, to close the dialog with no change.
5. Repeat the same check under **Preferences → Manage Footprint Libraries**,
   on its own **Project Specific Libraries** tab.
6. Find the `U1` symbol on your sheet. Confirm it still shows as a normal
   TMC6300-LA block, with no pink outline and no missing-library warning.

Your own computer may also have a matching library, added earlier, under
**Global Libraries**. If so, KiCad may show a naming note between the two.
This note is expected, and is not an error. The project-specific entry, now
committed to the repository, is the entry that travels with the project.

---

## 7. Saving and committing your work

KiCad and `git` are two separate tools, and each needs its own save step.

1. In whichever KiCad window you edited, press `Ctrl+S`, to save that file to
   disk. Do this often, not only at the end of a session.
2. Once you have made a meaningful change, for example finishing the wiring
   of one part, open a terminal in your local copy of this repository.
   Then run:
   ```bash
   git status
   ```
   This command lists every changed file. Confirm the files you expect to
   see, for example `home-controller-daughterboard.kicad_sch`, appear in this
   list.
3. Stage and commit your change:
   ```bash
   git add pcb/home-controller-daughterboard/
   git commit -m "Describe what you changed here"
   ```
4. Push the commit, so it reaches the shared copy of this repository:
   ```bash
   git push
   ```

A change that exists only in an unsaved KiCad window is not yet part of
this project, in the sense this guide has used throughout. The same is true
of a change that exists only in an uncommitted local file. Neither change
sits yet where a fresh clone, or a future review, can see it.

---

## 8. The Schematic Editor screen, and the actions you will use constantly

### 8.1 Screen layout

- The large white or black area in the middle is your drawing sheet.
- The left-hand toolbar holds drawing tools: place a symbol, draw a wire,
  place a label, and so on.
- The right-hand panel shows the symbols already on your sheet, in a tree
  list.
- The top toolbar holds file actions, plus the two buttons you will use most
  after every editing session: the electrical-rules-check button, and the
  "Update PCB from Schematic" button.

### 8.2 The actions you will repeat constantly

| Action | Toolbar icon | Keyboard shortcut |
|---|---|---|
| Place a symbol | An icon that looks like a chip outline with a plus sign | `A` |
| Draw a wire | A diagonal line icon | `W` |
| Place a label | A label-tag icon | `L` |
| Place a power symbol (GND, +3.3V) | A ground-symbol icon | `P` |
| Place a "no connect" flag | An X-in-a-box icon | `Q` |
| Move a selected item | — | `M` |
| Rotate a selected item | — | `R` |
| Mirror a selected item | — | `X` or `Y` |
| Delete a selected item | — | `Delete` |
| Undo | — | `Ctrl+Z` |
| Save | — | `Ctrl+S` |
| Zoom to fit everything on screen | — | `Ctrl+0` or the Home key |

Save your work often, with `Ctrl+S`. KiCad does not auto-save every change.

---

## 9. Setting up further footprint libraries, for parts you have not placed yet

Do this step once for each `.pretty` folder you want to reuse. Skip this
step for a symbol you already placed, since that already works.

The SmartKnob reference project, at
`docs/smartknob-repo/electronics/lib/`, contains several folders that end in
`.pretty`. Each of these folders is a real, modern footprint library. You can
add these folders directly, with no conversion needed.

1. In the Schematic Editor, or the PCB Editor, open the **Preferences** menu.
2. Select **Manage Footprint Libraries**.
3. A table appears, with a **Project Specific Libraries** tab and a
   **Global Libraries** tab. Select the **Project Specific Libraries** tab,
   so the new libraries apply only to this project.
4. Select the small folder icon, or the "+" button, below the table, to add a
   new row.
5. In the new row, set the **Library Path** field to the full path of one
   `.pretty` folder, for example
   `docs/smartknob-repo/electronics/lib/strain.pretty`. Give the row a short
   **Nickname**, for example `strain`.
6. Repeat step 5 for each `.pretty` folder you plan to use. At minimum, add
   `strain.pretty`, `sk6812.pretty`, `VEML7700.pretty`, `Holes.pretty`, and
   `SolderPads.pretty`.
7. Select **OK** to save the table.

You now have these footprints available, by their nickname, whenever you
assign a footprint to a symbol later in this guide.

---

## 10. Reusing a SmartKnob symbol, step by step

The SmartKnob reference schematic already contains working symbols for
several parts on your list: the 3.3 V regulator, the USB-C receptacle, and
the SK6812 LED. The HX711, the RP2040, and the MT6701QT in your project
already come from the built-in libraries of KiCad. You do not need this
method for those three parts.

1. Open a second, separate Schematic Editor window. In the Schematic Editor,
   open **File → Open**, and open
   `docs/smartknob-repo/electronics/view_base/view_base.kicad_sch`. This
   opens a second window, next to the window for your own project.
2. In the SmartKnob window, find the part you want. Use `Ctrl+F` to search
   by name, for example `TS1117BCW33_RPG` for the 3.3 V regulator, or
   `USB_C_Receptacle` for the connector.
3. Left-click once on the outline of the symbol, to select it. A selected
   symbol turns a highlight colour.
4. Press `Ctrl+C` to copy the symbol.
5. Switch to the window for your own project.
6. Press `Ctrl+V` to paste. The symbol follows your mouse cursor.
7. Left-click on an empty area of your sheet, to place the symbol there.

KiCad copies the full definition of the symbol along with it. This symbol
now works in your project, with no missing-library error. This is true even
though the original `.lib` file no longer exists, the file the SmartKnob
library table still names. This is the same kind of gap that Section 5
fixed for your own TMC6300-LA part, so the pattern should already look
familiar.

After pasting, check two items on the new symbol:

- **Reference**: KiCad may assign a reference like `U9`, copied from the
  SmartKnob sheet. Right-click the symbol, select **Properties**, and check
  the **Reference** field does not clash with a reference you already used.
  KiCad also flags a clash automatically, the next time you run the
  electrical rules check in Section 14.
- **Footprint**: the **Footprint** field in the same Properties window may
  still point at a SmartKnob library nickname that does not exist in your
  project yet. Fix this in Section 13, once your footprint libraries are set
  up as in Section 9.

Repeat this copy-paste process for each part in the table below.

| Part you need | Search this name in the SmartKnob window |
|---|---|
| 3.3 V regulator | `TS1117BCW33_RPG` |
| USB-C receptacle | `USB_C_Receptacle_USB2.0` |
| SK6812 LED (place one, then copy it 15 more times for your 16 LEDs) | `SK6812SIDE-A` |
| VEML7700 ambient sensor | `VEML7700` |
| Small ceramic capacitor, for every decoupling cap in this guide | `C_Small` |
| Small inductor, for the TMC6300 VM filter | `L_Small` |
| Small resistor, for every pull-up and pull-down in this guide | `R_Small` |
| Mounting hole | `MountingHole_Pad` |

Do not copy `CH340C`, `T-Micro32_Plus`, `MT6701-CT`, or `SN74LV1T34DBV`. Each
of these belongs to a part choice SmartKnob made that this project does not
use. Read [DESIGN_NOTES.md](DESIGN_NOTES.md) for the parts this project uses
instead.

---

## 11. Placing a brand-new symbol, for a part not in the SmartKnob file

Use this method for the MAX17048. Also use this method for a generic part,
for example a crystal, a resistor, or a capacitor, that the table in
Section 10 does not cover.

1. In the Schematic Editor, press `A`, or select the place-symbol icon.
2. A symbol chooser window opens, with a search box at the top.
3. Type a search term, for example `Crystal`, or `MAX17048`.
4. The built-in libraries of KiCad already hold many generic parts,
   including a plain two-pin `Crystal` symbol, and generic `R`, `C`, and `L`
   symbols. A specific chip, such as the MAX17048, needs its own symbol,
   from the manufacturer or a verified external library, per
   [DESIGN_NOTES.md](DESIGN_NOTES.md). Download that symbol library first,
   from the manufacturer. Add the library under **Preferences → Manage
   Symbol Libraries**, on the Symbol Libraries table this time, instead of
   the Footprint Libraries table. Use the same steps as Section 9. Section 5
   already describes this same process in full, for the TMC6300-LA part in
   your project.
5. Select the correct symbol from the search results, then select **OK**.
6. The symbol follows your cursor. Left-click on your sheet to place it.
7. Press `R` to rotate the symbol, if you want its pins on a different side.

---

## 12. Wiring two pins together

A wire in KiCad only creates a connection where it visibly touches two pins,
or crosses another wire with a junction dot.

1. Press `W`, or select the draw-wire icon.
2. Left-click on the open end of the first pin. A small circle marks an
   unconnected pin.
3. Move your mouse to the second pin. KiCad draws the wire as you move.
4. Left-click on the open end of the second pin, to finish the wire.
5. Press `Escape` to stop drawing wires.

For a connection that repeats many times on one sheet, for example every
ground pin, use a label instead of a long wire:

1. Press `P`, or select the place-power-symbol icon, for a true power net
   like `GND`, `+3V3`, or `+5V`. Search for the matching power symbol, place
   it, then wire it to the pin exactly as in the steps above.
2. Press `L`, or select the place-label icon, for a plain signal name that is
   not a power rail, for example `STRAIN_DO`. Type the exact same label text
   at both ends of a connection, on any two points on the sheet. KiCad then
   treats those two points as connected, with no visible wire drawn between
   them. Your existing schematic already uses this method, for `STRAIN_DO`
   and `STRAIN_SCK`, between the HX711 and the border of the RP2040 sheet.

Use the exact net names from
[CLAUDE.md](../CLAUDE.md) and [KNOB-PARTS.md](KNOB-PARTS.md) for every
label, so a later reader can match your schematic against those documents.

---

## 13. Assigning a footprint to a symbol

Do this step for every symbol, once your footprint libraries from Section 9
are set up.

1. Right-click the symbol, and select **Properties**.
2. Find the **Footprint** field.
3. Select the small "..." button next to that field, to open the footprint
   chooser.
4. Search for a matching footprint. For a part from the reuse table in
   Section 10, the correct footprint usually shares a name with the symbol.
   Look inside the library nickname you gave it in Section 9, for example
   `strain:BF350-3AA`.
5. Select the correct footprint, then select **OK**, then **OK** again to
   close the Properties window.

Repeat this for every symbol on your sheet. A symbol with no footprint
assigned stops you from moving on to the PCB stage.

---

## 14. Running the Electrical Rules Check

The Electrical Rules Check, ERC for short, scans your whole schematic for a
mistake a computer can catch automatically: an unconnected pin, two outputs
tied together, or a duplicate reference.

1. Select the ERC icon in the top toolbar, or open **Inspect → Electrical
   Rules Checker**.
2. Select **Run ERC**.
3. A list of results appears, split into errors and warnings.
4. Double-click a result, to jump straight to that spot on your sheet.
5. Fix the issue, then run ERC again.

A pin you truly intend to leave unconnected still needs a mark, so ERC does
not keep flagging it. An example, in this design, is the `PUSH` pin of the
MT6701QT:

1. Press `Q`, or select the no-connect icon.
2. Left-click directly on the open pin.
3. A small X appears on that pin. ERC now treats this pin as intentionally
   unused.

Run ERC after every editing session, not only once at the end. A fresh error
is far easier to fix the same day you introduced it.

---

## 15. Splitting the schematic into multiple sheets (recommended, optional)

Your project has enough parts that one flat sheet becomes hard to read.
KiCad supports hierarchical sheets, which work like folders for your circuit.

1. Press `S`, or select the hierarchical-sheet icon.
2. Drag a rectangle on your top sheet. A dialog asks for a **Sheet name**
   and a **File name**. Use a name from
   [KNOB-PARTS.md](KNOB-PARTS.md), for example `Motor Drive`.
3. Double-click the new rectangle, to enter that sheet. This opens a blank
   sheet, where you place the symbols for that section only.
4. To carry a net between sheets, place a **hierarchical label** inside the
   sub-sheet. Press `Ctrl+H`, or select the hierarchical-label icon. Type the
   exact net name, for example `GND` or `KNOB_UART_TX`.
5. Return to the top sheet. The rectangle now shows a matching pin for each
   hierarchical label you added. Wire that pin like any other pin.

A reasonable split for this project, matching
[KNOB-PARTS.md](KNOB-PARTS.md): Power, RP2040 Core, Motor Drive, Angle
Sensing, Press Sensing, Buttons, Lighting, and Ambient/Battery Sensing.

This step is optional. A flat single sheet still works electrically. Splitting
only makes a large design easier to read and debug.

---

## 16. Moving from the schematic to the PCB

Once ERC reports zero errors, and every symbol has a footprint, move to the
PCB Editor.

1. From the Schematic Editor, select **Tools → Update PCB from Schematic**,
   or press `F8`.
2. A dialog compares your schematic against the current PCB file, and lists
   every part it will add. Select **Update PCB**.
3. Switch to the PCB Editor window. Every part now appears, piled near one
   corner of the board, connected by thin white lines called the **ratsnest**.
   Each ratsnest line marks a connection your schematic requires, that the
   PCB copper does not yet provide.

Every time you change the schematic afterward, repeat this Update PCB step,
to bring the PCB file back in sync. Your PCB file currently holds no parts at
all, so its first Update PCB step will add every part from your schematic at
once.

---

## 17. Drawing the board outline

1. In the PCB Editor, select the **Edge.Cuts** layer, from the layer list on
   the right-hand side of the screen.
2. Select the line- or rectangle-drawing tool from the left-hand toolbar.
3. Draw the outer shape of your daughterboard on this layer. This shape
   becomes the physical edge of the finished board.

Keep this outline simple for a first board: a plain rectangle, sized to fit
around your knob mechanism, is enough to start routing. Refine the exact
shape later, once you have placed every part and know how much space you
actually need.

---

## 18. Placing footprints on the board

1. Left-click and drag each footprint from the pile, to a rough position on
   your board outline.
2. Press `R` to rotate a footprint, so its pins face the right direction for
   your planned wiring.
3. Place the highest-current parts first: the TMC6300, the motor connector,
   and the LiPo battery pads. Keep these three close together, since the
   motor phase traces between them carry the most current on this board.
4. Place the MT6701QT close to the physical shaft and the magnet position.
   This sensor must sit directly above the magnet, to read the angle
   correctly.
5. Place the RP2040, its crystal, and its flash chip close together, since
   these three parts connect with short, sensitive high-speed signals.

---

## 19. Routing copper traces

1. Press `X`, or select the route-tracks icon.
2. Left-click on a pad, then move your mouse toward the pad the ratsnest
   line points to. KiCad draws a copper trace, following your mouse, snapped
   to the grid.
3. Left-click again, at the destination pad, to finish that trace. The
   ratsnest line for that connection disappears once the trace fully
   connects both ends.
4. Before you route the six motor-phase traces to the TMC6300, and the two
   LiPo power traces, widen the track. Select **Route → Interactively Route
   Single Track**, or type a specific width into the track-width box in the
   top toolbar, before you start that trace. A wider track carries more
   current safely. Check the current rating in the TMC6300 datasheet, and a
   PCB trace-width calculator, before you confirm an exact width figure. The
   correct width depends on the copper thickness your fabricator uses.
5. Route every remaining signal at the default track width of the board,
   unless the datasheet for a specific part states otherwise.

For the ground connection, most boards use a filled copper area instead of
individual traces:

1. Select the **B.Cu** layer (the bottom copper layer of the board, a common
   choice for a ground pour).
2. Select the **Add Filled Zone** tool, from the left-hand toolbar.
3. Draw a rectangle around your whole board outline.
4. In the dialog that appears, set the **Net** field to `GND`.
5. Select **OK**. KiCad fills the whole area with copper, except where a
   trace or a pad of a different net needs clearance.
6. Right-click the new zone, and select **Fill All Zones**, whenever you want
   to see or refresh the filled copper.

---

## 20. Running the Design Rules Check

The Design Rules Check, DRC for short, is the PCB Editor equivalent of ERC.
DRC catches a physical mistake: two traces placed too close together, a
trace with no connection, or a footprint overlapping the board edge.

1. Select **Inspect → Design Rules Checker**.
2. Select **Run DRC**.
3. Fix every reported error, the same way you fixed ERC errors: double-click
   a result to jump to that spot, fix it, then run DRC again.

Do not send a board to a fabricator while DRC reports an unresolved error.

---

## 21. Generating the files a fabricator needs

Once DRC reports zero errors, generate the manufacturing files:

1. Select **File → Fabrication Outputs → Gerbers**.
2. In the dialog, confirm every copper, silkscreen, solder-mask, and
   edge-cuts layer your fabricator needs is checked, then select **Plot**.
3. In the same dialog, select **Generate Drill Files**, to create the file
   that tells the fabricator where to drill every hole.
4. Collect every generated file into one folder, or one zip archive.

Most fabricators accept this same Gerber-and-drill-file format. The exact
upload process differs by fabricator. Check the instructions of your chosen
fabricator, for the exact file-naming convention they expect.

Keep these output files separate from `pcb/home-controller-daughterboard/`,
and do not commit them to the repository. A fabrication output regenerates
at any time from the committed `.kicad_pcb` file, so it carries no
information the repository does not already have.

---

## 22. A troubleshooting list, for common first-time issues

- **A pink or red outline, or a "footprint not found" error**: the
  footprint field points at a library nickname your project does not have.
  Revisit Section 9, then Section 13.
- **ERC reports "Pin not driven" on a pin you meant to leave alone**: add a
  no-connect flag, from Section 14.
- **A ratsnest line remains after you routed a trace**: the trace likely
  stopped short of the pad, instead of landing exactly on it. Zoom in, and
  redraw the last segment directly onto the centre of the pad.
- **You cannot find a menu item exactly as this guide names it**: KiCad
  reorganizes menus slightly between versions. Use the search box inside the
  **Preferences → Hotkeys** table. You can also hover over each toolbar
  icon, to find the closest match to the action this guide describes.
- **You made a mistake and want to start a step over**: press `Ctrl+Z`
  repeatedly. The undo history of KiCad covers most actions in both editors.
- **You are not sure if you saved**: check the title bar of the project
  manager window, and the title bar of each open editor window. Either bar
  shows an asterisk next to the file name, when unsaved changes exist. Press
  `Ctrl+S` in that window to clear it.
- **A part shows correctly for you, but a fresh clone reports it missing**:
  the part most likely comes from a library added under **Global
  Libraries**, on your own computer, instead of **Project Specific
  Libraries**, inside the repository. Revisit Section 5 for the reasoning,
  and Section 9 for the general steps.

---

## 23. How this guide maps to the work already recorded

[DESIGN_NOTES.md](DESIGN_NOTES.md) and [KNOB-PARTS.md](KNOB-PARTS.md) already
record every part this board needs, and every decision still open. Use the
sections of this guide in this order, for a first working schematic:

1. Sections 1 through 7, once, for your project setup.
2. Section 9, for the SmartKnob footprint libraries you plan to use.
3. Section 10, for every part the SmartKnob file can supply.
4. Section 11, for the RP2040 support parts and the MAX17048.
5. Section 12 and Section 13, for every symbol already on your sheet, and
   every symbol you add.
6. Section 14, until it reports zero errors.
7. Section 15, if the sheet becomes hard to read.
8. Sections 16 through 20, for the physical board.
9. Section 21, only once every earlier section is complete.
