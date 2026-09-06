# KiCad Guide: Building the Haptic-Knob Daughterboard

This guide is for a first-time KiCad user. This guide assumes no prior
electronic-design-automation experience. This guide targets KiCad 10, since
your existing schematic already uses that version.

Read [DESIGN_NOTES.md](DESIGN_NOTES.md) for the hardware decisions this guide
assumes. Read [KNOB-PARTS.md](KNOB-PARTS.md) for the full parts list. This
guide explains how to turn those decisions into a finished circuit board, one
click at a time.

---

## 1. What each KiCad file and window actually is

A KiCad project has three main files, all with the same base name:

- `home-controller-daughterboard.kicad_pro`: the project file. Double-click
  this file to open the project.
- `home-controller-daughterboard.kicad_sch`: the schematic. This file records
  which parts exist, and how their pins connect to each other, as a diagram.
- `home-controller-daughterboard.kicad_pcb`: the PCB layout. This file records
  the physical copper board: part positions, copper traces, and drill holes.

Two separate editor windows work on these files:

- **Schematic Editor** (KiCad calls this window Eeschema internally, but the
  KiCad 10 project manager labels the button "Schematic Editor"). Opens the
  `.kicad_sch` file. You draw your circuit diagram here first.
- **PCB Editor** (Pcbnew internally, labelled "PCB Editor" in the project
  manager). Opens the `.kicad_pcb` file. You lay out the physical board here,
  after the schematic is complete.

Two more terms matter throughout this guide:

- A **symbol** is the diagram drawing of a part, in the Schematic Editor: a
  rectangle with labelled pins, for example the TMC6300 box in your
  screenshot.
- A **footprint** is the physical pad pattern for that same part, in the PCB
  Editor: the actual copper pads a real chip solders onto.

Every symbol links to exactly one footprint. KiCad keeps the two separate on
purpose, since one symbol (for example, a generic resistor) can use many
different footprints (many different physical sizes).

---

## 2. The Schematic Editor screen, and the actions you will use constantly

Open your `home-controller-daughterboard.kicad_sch` file now, in the
Schematic Editor, so you can follow along.

### 2.1 Screen layout

- The large white or black area in the middle is your drawing sheet.
- The left-hand toolbar holds drawing tools: place a symbol, draw a wire,
  place a label, and so on.
- The right-hand panel shows the symbols already on your sheet, in a tree
  list.
- The top toolbar holds file actions, plus the two buttons you will use most
  after every editing session: the electrical-rules-check button, and the
  "Update PCB from Schematic" button.

### 2.2 The actions you will repeat constantly

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

## 3. Setting up your footprint libraries first

Do this step once, before you place any new part. Skip this step for a
symbol you already placed, such as your TMC6300, RP2040, MT6701QT, and
HX711 boxes, since those already work.

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

## 4. Reusing a SmartKnob symbol, step by step

The SmartKnob reference schematic already contains working symbols for
several parts on your list: the 3.3 V regulator, the USB-C receptacle, the
SK6812 LED, the VEML7700, and the HX711. You already correctly placed the
HX711 symbol yourself, so use this method for the parts you have not placed
yet.

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
library table still names.

After pasting, check two items on the new symbol:

- **Reference**: KiCad may assign a reference like `U9`, copied from the
  SmartKnob sheet. Right-click the symbol, select **Properties**, and check
  the **Reference** field does not clash with a reference you already used.
  KiCad also flags a clash automatically, the next time you run the
  electrical rules check in Section 8.
- **Footprint**: the **Footprint** field in the same Properties window may
  still point at a SmartKnob library nickname that does not exist in your
  project yet. Fix this in Section 6, once your footprint libraries are set
  up as in Section 3.

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

## 5. Placing a brand-new symbol, for a part not in the SmartKnob file

Use this method for the RP2040, the MT6701QT, and the MAX17048. You already
placed the RP2040 and the MT6701QT symbols correctly. Use this method mainly
for the MAX17048. Also use this method for a generic part, for example a
crystal, a resistor, or a capacitor, that the table in Section 4 does not
cover.

1. In the Schematic Editor, press `A`, or select the place-symbol icon.
2. A symbol chooser window opens, with a search box at the top.
3. Type a search term, for example `Crystal`, or `MAX17048`.
4. The built-in libraries of KiCad already hold many generic parts,
   including a plain two-pin `Crystal` symbol, and generic `R`, `C`, and `L`
   symbols. A specific chip, such as the MAX17048, needs its own symbol,
   from the manufacturer or a verified external library, per
   [DESIGN_NOTES.md](DESIGN_NOTES.md). Download that symbol library first,
   from the manufacturer. Add the library under **Preferences → Manage
   Symbol Libraries**. Use the same steps as Section 3, but on the Symbol
   Libraries table, instead of the Footprint Libraries table.
5. Select the correct symbol from the search results, then select **OK**.
6. The symbol follows your cursor. Left-click on your sheet to place it.
7. Press `R` to rotate the symbol, if you want its pins on a different side.

---

## 6. Wiring two pins together

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

## 7. Assigning a footprint to a symbol

Do this step for every symbol, once your footprint libraries from Section 3
are set up.

1. Right-click the symbol, and select **Properties**.
2. Find the **Footprint** field.
3. Select the small "..." button next to that field, to open the footprint
   chooser.
4. Search for a matching footprint. For a part from the reuse table in
   Section 4, the correct footprint usually shares a name with the symbol.
   Look inside the library nickname you gave it in Section 3, for example
   `strain:BF350-3AA`.
5. Select the correct footprint, then select **OK**, then **OK** again to
   close the Properties window.

Repeat this for every symbol on your sheet. A symbol with no footprint
assigned stops you from moving on to the PCB stage.

---

## 8. Running the Electrical Rules Check

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

## 9. Splitting the schematic into multiple sheets (recommended, optional)

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

## 10. Moving from the schematic to the PCB

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
to bring the PCB file back in sync.

---

## 11. Drawing the board outline

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

## 12. Placing footprints on the board

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

## 13. Routing copper traces

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

## 14. Running the Design Rules Check

The Design Rules Check, DRC for short, is the PCB Editor equivalent of ERC.
DRC catches a physical mistake: two traces placed too close together, a
trace with no connection, or a footprint overlapping the board edge.

1. Select **Inspect → Design Rules Checker**.
2. Select **Run DRC**.
3. Fix every reported error, the same way you fixed ERC errors: double-click
   a result to jump to that spot, fix it, then run DRC again.

Do not send a board to a fabricator while DRC reports an unresolved error.

---

## 15. Generating the files a fabricator needs

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

---

## 16. A troubleshooting list, for common first-time issues

- **A pink or red outline, or a "footprint not found" error**: the
  footprint field points at a library nickname your project does not have.
  Revisit Section 3, then Section 7.
- **ERC reports "Pin not driven" on a pin you meant to leave alone**: add a
  no-connect flag, from Section 8.
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

---

## 17. How this guide maps to the review already recorded

[DESIGN_NOTES.md](DESIGN_NOTES.md) and [KNOB-PARTS.md](KNOB-PARTS.md) already
record every part this board needs, and every decision still open. Use the
sections of this guide in this order, for a first working schematic:

1. Section 3, once, for your footprint libraries.
2. Section 4, for every part the SmartKnob file can supply.
3. Section 5, for the RP2040 support parts and the MAX17048.
4. Section 6 and Section 7, for every symbol already on your sheet, and
   every symbol you add.
5. Section 8, until it reports zero errors.
6. Section 9, if the sheet becomes hard to read.
7. Sections 10 through 14, for the physical board.
8. Section 15, only once every earlier section is complete.
