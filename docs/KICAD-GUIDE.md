# KiCad Guide: Building the Haptic-Knob Daughterboard

This guide assumes no memory of KiCad, and no prior electronic-design
experience. This guide starts from installing the software, and ends with a
manufacturable board. Read this guide in order, the first time through.

This guide targets KiCad 10, since your project already uses that version.

Read [DESIGN_NOTES.md](DESIGN_NOTES.md) for the hardware decisions this guide
assumes. Read [KNOB-PARTS.md](KNOB-PARTS.md) for the full parts list.

---

## 0. Where each part of this guide happens

Three separate places matter to this guide, and every step below names
which place it happens in:

- **Your own computer.** This is where KiCad runs. This is also where your
  normal working copy of this repository already lives, at
  `C:\Users\User\Documents\home-controller`. Every KiCad step in this guide
  runs here. Every `git` command in this guide also runs here, in a
  terminal on your own computer.
- **GitHub.** This is the shared copy of this repository, on the internet.
  A `git push`, from your own computer, sends your work here. A `git pull`,
  on your own computer, brings a change back down from here.
- **A cloud session, like the one that wrote this guide**: a cloud session
  works inside a separate, temporary copy of this repository, with no KiCad
  installed. A cloud session can read and write the plain text of a KiCad
  file. A cloud session can also run `git` commands against GitHub
  directly. A cloud session never opens the KiCad application itself, and
  never touches your own computer.

Two files, `sym-lib-table` and `fp-lib-table`, from this guide, already
reached GitHub this way, from a cloud session, in the same manner Section 4
describes. Those two files are not yet on your own computer, until you pull
them down there, with the steps in Section 4.

From this point on, every step in this guide happens on your own computer,
unless the step says otherwise.

---

## 1. Install KiCad

1. On your own computer, go to `kicad.org`, and download the installer for
   Windows.
2. Run the installer, and accept its default options, unless you have a
   specific reason to change one. The default install needs a few hundred
   megabytes of free disk space.
3. Install version 10. An older version may open this project with a
   warning, or may not open it at all.
4. Launch KiCad once, after installation. KiCad opens its **KiCad Project
   Manager** window first. This window lists your recent projects, and lets
   you create or open a project.
5. Confirm the installed version. Select **Help → About KiCad**, in the
   Project Manager window. Confirm the version number begins with `10`.

Leave the Project Manager window open; Section 5 uses it.

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
  project where to find a custom part. Section 6 explains these two files in
  full.

Three separate editor windows work on these files:

- **Schematic Editor**. Opens the `.kicad_sch` file. You draw your circuit
  diagram here first.
- **PCB Editor**. Opens the `.kicad_pcb` file. You lay out the physical board
  here, after the schematic is complete.
- **3D Viewer**: a read-only window, opened from inside the PCB Editor, with
  **View → 3D Viewer**. This window renders your board as a realistic 3D
  model. This render shows a real shape only for a footprint that carries
  its own 3D model. Your TMC6300-LA part is one such example, once that
  footprint sits on the board.

Two more terms matter throughout this guide:

- A **symbol** is the diagram drawing of a part, in the Schematic Editor: a
  rectangle with labelled pins.
- A **footprint** is the physical pad pattern for that same part, in the PCB
  Editor: the actual copper pads a real chip solders onto.

Every symbol links to exactly one footprint. KiCad keeps the two separate on
purpose. One symbol, for example a generic resistor, can use many different
footprints, for many different physical part sizes.

One display setting matters from the start: KiCad measures in millimetres by
default, in both editors. Every measurement in this guide assumes
millimetres, unless stated otherwise. Change this setting under
**View → Units**, if you prefer inches. Both editors also carry unit
buttons in the left-hand toolbar, for the same switch. Keep the whole
project on one unit system throughout, to avoid a misread measurement.

---

## 3. Where this project lives, on your computer and on GitHub

This project already exists in this repository, at this exact path:

```
pcb/home-controller-daughterboard/
```

On your own computer, this same path sits inside your existing working copy
of this repository, so the full path is:

```
C:\Users\User\Documents\home-controller\pcb\home-controller-daughterboard\
```

This is the correct location for it, and the reason matters for how you work
from here on. A fresh copy of this repository needs every file that opens
and builds this board correctly. Each such file lives inside the repository
itself, checked in with `git`, not only saved on your own computer. This
single rule answers both of your questions at once: where to save your
work, and how another reader, including a future cloud session, can check
it. A future session reads the plain text of every file already committed
to this repository. A file that exists only on your own computer, and was
never committed, stays invisible to that session. This same file stays
invisible to any other computer that clones this repository fresh.

The `pcb/home-controller-daughterboard/` folder currently holds:

- The four project files from Section 2.
- `sym-lib-table` and `fp-lib-table`, added by the cloud session in Section 6.

One more folder matters to this same project, even though it sits in a
different part of the repository:

```
C:\Users\User\Documents\home-controller\docs\Symbols & Footprints\TMC6300-LA\
```

This folder holds a custom part: the exact TMC6300-LA symbol, footprint,
and 3D model that the `U1` component in your schematic already uses. This
part came from a component search site named SnapEDA. This folder was
already committed to the repository, but the new `sym-lib-table` and
`fp-lib-table` files, from Section 6, are what actually connect your
project to it.

---

## 4. Getting today's fix onto your computer

A cloud session already committed the two new files from Section 3 to
GitHub. Bring them down to your own computer with these steps, run on your
own computer, before you open the project in KiCad:

1. Open a terminal on your own computer. PowerShell is the terminal the
   other guides in this project already use.
2. Move to your existing working copy of this repository:
   ```powershell
   Set-Location "C:\Users\User\Documents\home-controller"
   ```
3. Confirm you have no uncommitted local change, before you pull. Run:
   ```powershell
   git status
   ```
   A clean result reads `nothing to commit, working tree clean`. If this
   command instead lists a changed file you did not expect, stop, and work
   out what that change is, before you continue. Pulling over an
   uncommitted change can be confusing to unpick later.
4. Bring the new commits down from GitHub:
   ```powershell
   git pull
   ```
5. Confirm the new files arrived:
   ```powershell
   Test-Path "pcb\home-controller-daughterboard\sym-lib-table"
   Test-Path "pcb\home-controller-daughterboard\fp-lib-table"
   ```
   Both commands should print `True`.

Repeat this same pull step at the start of any future KiCad session. A
cloud session, or work from another computer, may have added a commit since
you last checked.

---

## 5. Opening the project for the first time

1. Open the KiCad Project Manager, on your own computer.
2. Select **File → Open Project**.
3. Navigate to
   `C:\Users\User\Documents\home-controller\pcb\home-controller-daughterboard\`.
4. Select `home-controller-daughterboard.kicad_pro`, then select **Open**.
5. The Project Manager now shows this project, with the Schematic Editor and
   the PCB Editor available as buttons, in the main part of the window.
6. Select **Schematic Editor**, to open your existing circuit diagram.

---

## 6. The library gap this session closed

Before this fix, the `U1` symbol in your project, the TMC6300-LA, worked on
your own computer. This symbol likely would not have opened correctly on a
different computer, including a fresh clone of this repository. The reason:
KiCad finds a custom symbol through a library table, and your project had
no project-specific table of its own. Your copy of KiCad most likely found
this part a different way instead. This other way was a library added to
the global settings of your own computer, outside this repository. This
part therefore stayed invisible to anyone else.

This fix added two missing files, directly inside
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

## 7. Confirming the fix

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

## 8. Saving and committing your work

KiCad and `git` are two separate tools, and each needs its own save step.
Both run on your own computer, in the same working copy of this repository
that Section 4 already set up.

1. In whichever KiCad window you edited, press `Ctrl+S`, to save that file to
   disk. Do this often, not only at the end of a session.
2. Wait until you have made a meaningful change, for example finishing the
   wiring of one part. Then open PowerShell, and move to your working copy,
   the same way as Section 4:
   ```powershell
   Set-Location "C:\Users\User\Documents\home-controller"
   ```
3. List what changed:
   ```powershell
   git status
   ```
   This command lists every changed file. Confirm the files you expect to
   see, for example `home-controller-daughterboard.kicad_sch`, appear in this
   list. Run `git diff` for a text-based project file, such as
   `sym-lib-table`, to see the exact line-by-line change; a `.kicad_sch` or
   `.kicad_pcb` file is also plain text underneath, though its content is
   dense enough that `git status` alone is usually the more useful check.
4. Stage and commit your change:
   ```powershell
   git add pcb\home-controller-daughterboard\
   git commit -m "Describe what you changed here"
   ```
   Write the commit message in the same style the rest of this project
   already uses: a short, specific sentence, for example
   `Wire the TMC6300 VCP and 1.8VOUT decoupling caps`.
5. Push the commit, so it reaches GitHub:
   ```powershell
   git push
   ```
   The normal rule for this project, for your own solo work, is to push
   directly to `main`. `CLAUDE.md`, in the repository root, records this
   rule in full, alongside every other project-wide git rule.

A change that exists only in an unsaved KiCad window is not yet part of
this project, in the sense this guide has used throughout. The same is true
of a change that exists only in an uncommitted local file. Neither change
sits yet where a fresh clone, or a future review, can see it.

One more benefit of committing often: `git` becomes a safety net against a
mistake. Suppose you delete something in KiCad by accident, and already
saved that mistake to disk. Run `git diff`, to see exactly what changed
since your last commit. Run `git checkout -- <file>`, to restore that file
to its last committed state, undoing every change since then, including the
mistake.

---

## 9. The Schematic Editor screen, and the actions you will use constantly

### 9.1 Screen layout

- The large white or black area in the middle is your drawing sheet.
- The left-hand toolbar holds drawing tools: place a symbol, draw a wire,
  place a label, and so on.
- The right-hand panel shows the symbols already on your sheet, in a tree
  list.
- The top toolbar holds file actions, plus the two buttons you will use most
  after every editing session: the electrical-rules-check button, and the
  "Update PCB from Schematic" button.
- A faint dot grid covers the sheet. KiCad snaps every symbol, wire, and
  label to this grid, so two items that look aligned actually are aligned.
  Change the grid spacing from the grid drop-down list, in the top
  toolbar. Do this whenever the default spacing feels too coarse, or too
  fine, for a specific part of your drawing.

### 9.2 Moving around the sheet

- Scroll your mouse wheel to zoom in and out, centred on your cursor.
- Hold the middle mouse button, or the scroll wheel itself, and drag, to pan
  the view.
- Press `Ctrl+0`, or the Home key, to zoom out until the whole sheet fits on
  screen. Use this often, to re-orient yourself after zooming in on one
  detail.

### 9.3 The actions you will repeat constantly

| Action | Toolbar icon | Keyboard shortcut |
|---|---|---|
| Place a symbol | An icon that looks like a chip outline with a plus sign | `A` |
| Draw a wire | A diagonal line icon | `W` |
| Place a label | A label-tag icon | `L` |
| Place a power symbol (GND, +3.3V) | A ground-symbol icon | `P` |
| Place a "no connect" flag | An X-in-a-box icon | `Q` |
| Move a selected item | — | `M` |
| Drag a selected item, keeping its wires attached | — | `G` |
| Open the properties of a selected item | — | `E` |
| Rotate a selected item | — | `R` |
| Mirror a selected item | — | `X` or `Y` |
| Delete a selected item | — | `Delete` |
| Find a symbol or a label by name | — | `Ctrl+F` |
| Cancel the current tool | — | `Escape` |
| Undo | — | `Ctrl+Z` |
| Save | — | `Ctrl+S` |
| Zoom to fit everything on screen | — | `Ctrl+0` or the Home key |

Save your work often, with `Ctrl+S`. KiCad does not auto-save every change.

---

## 10. Setting up further footprint libraries, for parts you have not placed yet

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
   so the new libraries apply only to this project. Section 6 already used
   this same tab, for your TMC6300-LA part; adding another row here works
   the same way.
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

## 11. Reusing a SmartKnob symbol, step by step

Use this section for exactly two parts: the SK6812 LED, and the VEML7700
ambient sensor. Only these two come from a custom SmartKnob symbol library.

Every other part in this design comes from a library that KiCad already
installs on every computer. Place each of those with Section 12 instead,
which is a shorter and simpler method. The table at the end of this section
names which method each part needs, so check that table before you start.

1. Open a second, separate Schematic Editor window. In the Schematic Editor,
   open **File → Open**, and open
   `docs/smartknob-repo/electronics/view_base/view_base.kicad_sch`. This
   opens a second window, next to the window for your own project.
2. In the SmartKnob window, find the part you want. Use `Ctrl+F` to search
   by name: `SK6812SIDE-A` for the LED, or `VEML7700` for the ambient
   sensor.
3. Left-click once on the outline of the symbol, to select it. A selected
   symbol turns a highlight colour.
4. Press `Ctrl+C` to copy the symbol.
5. Switch to the window for your own project.
6. Press `Ctrl+V` to paste. The symbol follows your mouse cursor.
7. Left-click on an empty area of your sheet, to place the symbol there.

KiCad copies the full definition of the symbol along with it. This symbol
now works in your project, with no missing-library error. This is true even
though the original `.lib` file no longer exists, the file the SmartKnob
library table still names. This is the same kind of gap that Section 6
fixed for your own TMC6300-LA part, so the pattern should already look
familiar.

After pasting, check three items on the new symbol. Right-click the symbol,
and select **Properties**, to see all three fields at once:

- **Reference**: KiCad may assign a reference like `U9`, copied from the
  SmartKnob sheet. Check the **Reference** field does not clash with a
  reference you already used. KiCad also flags a clash automatically, the
  next time you run the electrical rules check in Section 16.
- **Value**: this field usually names the part, for example `SK6812SIDE-A`.
  Leave this field as it is, unless you have a specific reason to relabel
  the part.
- **Footprint**: this field may still point at a SmartKnob library nickname
  that does not exist in your project yet. Fix this in Section 14, once
  your footprint libraries are set up as in Section 10.

Place one SK6812 symbol with this method. Then copy that placed symbol 15
more times, with `Ctrl+C` and `Ctrl+V`, inside your own sheet. This gives
the full set of 16 LEDs: 12 for the ring, and 4 for the buttons.

This table names the correct method for every part in this design:

| Part you need | Where the symbol comes from | Method |
|---|---|---|
| SK6812 LED, 16 in total | Custom SmartKnob library | This section |
| VEML7700 ambient sensor | Custom SmartKnob library | This section |
| TMC6300-LA motor driver | Your own repository, already placed as `U1` | Already done, see Section 6 |
| RP2040, HX711, MT6701QT | Built into KiCad, already placed | Already done |
| 3.3 V regulator, `TS1117BCW33_RPG` | Built into KiCad, `Regulator_Linear` | Section 12 |
| USB-C receptacle, `USB_C_Receptacle_USB2.0` | Built into KiCad, `Connector` | Section 12 |
| Capacitor, resistor, inductor: `C_Small`, `R_Small`, `L_Small` | Built into KiCad, `Device` | Section 12 |
| Mounting hole, `MountingHole_Pad` | Built into KiCad, `Mechanical` | Section 12 |
| Crystal, MAX17048, MX switch, level shifter | Built into KiCad, or from the manufacturer | Section 12 |

The SmartKnob file also contains `CH340C`, `T-Micro32_Plus`, `MT6701-CT`,
and `SN74LV1T34DBV`. Do not copy any of these four. Each belongs to a part
choice SmartKnob made that this project does not use. Read
[DESIGN_NOTES.md](DESIGN_NOTES.md) for the parts this project uses instead.

---

## 12. Placing a brand-new symbol, for a part not in the SmartKnob file

Use this method for the MAX17048. Also use this method for a generic part,
for example a crystal, a resistor, or a capacitor, that the table in
Section 11 does not cover.

1. In the Schematic Editor, press `A`, or select the place-symbol icon.
2. A symbol chooser window opens, with a search box at the top. A preview
   pane, on the right, shows the symbol drawing. This same pane shows a
   small footprint preview, for many parts.
3. Type a search term, for example `Crystal`, or `MAX17048`.
4. The built-in libraries of KiCad already hold many generic parts,
   including a plain two-pin `Crystal` symbol, and generic `R`, `C`, and `L`
   symbols. A specific chip, such as the MAX17048, needs its own symbol,
   from the manufacturer or a verified external library, per
   [DESIGN_NOTES.md](DESIGN_NOTES.md). Download that symbol library first,
   from the manufacturer. Add the library under **Preferences → Manage
   Symbol Libraries**, on the Symbol Libraries table this time, instead of
   the Footprint Libraries table. Use the same steps as Section 10. Section 6
   already describes this same process in full, for the TMC6300-LA part in
   your project.
5. Select the correct symbol from the search results, then select **OK**.
6. The symbol follows your cursor. Left-click on your sheet to place it.
7. Press `R` to rotate the symbol, if you want its pins on a different side.

---

## 13. Wiring two pins together

A wire in KiCad only creates a connection at a point where it visibly
touches two pins. A wire also creates a connection where it crosses another
wire, at a junction dot: a small filled circle KiCad draws automatically,
where three or more wire ends meet.

1. Press `W`, or select the draw-wire icon.
2. Left-click on the open end of the first pin. A small circle marks an
   unconnected pin. Hover over a pin first, with no click, to see a
   tooltip. This tooltip names the pin number, and names the electrical
   type of that pin, for example an input pin, or a power-input pin.
3. Move your mouse to the second pin. KiCad draws the wire as you move.
4. Left-click on the open end of the second pin, to finish the wire.
5. Press `Escape` to stop drawing wires.

Left-click once on any finished wire, at any point, to select that whole
electrical net. Every pin, wire, and label KiCad considers part of the same
connection highlights together. Use this to check a connection you are
unsure about, before you rely on it.

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

## 14. Assigning a footprint to a symbol

Do this step for every symbol, once your footprint libraries from Section 10
are set up.

1. Right-click the symbol, and select **Properties**.
2. Find the **Footprint** field.
3. Select the small "..." button next to that field, to open the footprint
   chooser. This chooser shows a live preview of each footprint you
   highlight, on the right-hand side of the window. Confirm the pad shape
   and the pin count, in this preview, before you commit to a footprint.
4. Search for a matching footprint. For a part from the reuse table in
   Section 11, the correct footprint usually shares a name with the symbol.
   Look inside the library nickname you gave it in Section 10, for example
   `strain:BF350-3AA`.
5. Select the correct footprint, then select **OK**, then **OK** again to
   close the Properties window.

Repeat this for every symbol on your sheet. A symbol with no footprint
assigned stops you from moving on to the PCB stage.

---

## 15. Annotating the schematic

Every symbol needs a unique reference designator, for example `U1`, `R4`, or
`C12`. KiCad shows an unassigned reference as a question mark, for example
`R?`. A copied symbol can also arrive carrying a reference that another
symbol already uses. Annotation assigns every reference correctly, across
the whole schematic, in one action.

1. In the Schematic Editor, select **Tools → Annotate Schematic**.
2. Leave the default options selected, for a first run. The default keeps
   each existing reference that is already correct, and only fills in a
   missing or duplicated one.
3. Select **Annotate**.
4. Close the dialog.

Your four existing symbols, `U1` through `U4`, already carry a correct
reference each, so this step leaves them alone. Run this step again after
you add a group of new symbols, and always before the electrical rules
check in Section 16.

---

## 16. Running the Electrical Rules Check

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

### The power-flag error, and how to clear it

One ERC error catches almost every first-time KiCad user, and it looks more
alarming than it is. The text reads close to this:

```
Input Power pin not driven by any Output Power pin
```

This error does not mean your wiring is wrong. ERC traces each power net
back to a source that declares itself a power output. A battery pad, a
regulator output, or a bare connector pin often declares nothing at all.
ERC then decides the net has no source, even though the real board powers
that net correctly.

The fix is a `PWR_FLAG` symbol. This symbol connects to nothing physically,
and appears on no finished board. This symbol only tells ERC "a real supply
feeds this net."

1. Press `A`, or select the place-symbol icon.
2. Search for `PWR_FLAG`, and place one.
3. Wire it to the net ERC complained about, usually right at the point where
   power enters your board: the battery pad, the regulator output pin, or
   the USB-C power pin.
4. Run ERC again. The error for that net clears.

Add one `PWR_FLAG` per independent supply net. This design needs one on the
raw battery net, one on the 3.3 V rail, and one on the 5 V LED rail. The
SmartKnob reference schematic uses ten of these symbols, for the same
reason, so its sheet is a useful example to compare against.

### Excluding a result you have reviewed

A result you have reviewed, and judged acceptable exactly as it stands, can
be excluded, instead of fixed: right-click that result in the ERC list, and
select **Exclude this violation**. KiCad records this exclusion inside the
project file. ERC then does not raise the same result again, on your own
computer, or on any other computer that opens this same committed project.
Use this sparingly, and only after you understand exactly why ERC raised
the result in the first place.

Run ERC after every editing session, not only once at the end. A fresh error
is far easier to fix the same day you introduced it.

### Checking your schematic against the parts list

[KNOB-PARTS.md](KNOB-PARTS.md) records every part this board needs, with a
quantity for each. Export a parts list from your schematic, and compare the
two, to catch a part you forgot to place:

1. In the Schematic Editor, select **Tools → Generate Bill of Materials**.
2. Select an output format, then generate the file.
3. Compare each line against [KNOB-PARTS.md](KNOB-PARTS.md). Check the
   quantity of each repeated part especially: this design needs 16 SK6812
   LEDs, 4 BF350 strain gauges, 4 MX switches, and 2 level shifters.

Do this check once the schematic feels complete, and again before you order
a board.

---

## 17. Splitting the schematic into multiple sheets (recommended, optional)

Your project has enough parts that one flat sheet becomes hard to read.
KiCad supports hierarchical sheets, which work like folders for your circuit.

1. Select the hierarchical-sheet icon, in the left-hand toolbar. The
   **Place** menu holds this same action, if the icon is hard to find.
2. Drag a rectangle on your top sheet. A dialog asks for a **Sheet name**
   and a **File name**. Use a name from
   [KNOB-PARTS.md](KNOB-PARTS.md), for example `Motor Drive`.
3. Double-click the new rectangle, to enter that sheet. This opens a blank
   sheet, where you place the symbols for that section only.
4. To carry a net between sheets, place a **hierarchical label** inside the
   sub-sheet. Select the hierarchical-label icon, in the left-hand
   toolbar, or press `H`. Type the exact net name, for example `GND` or
   `KNOB_UART_TX`.
5. Return to the top sheet. The rectangle now shows a matching pin for each
   hierarchical label you added. Wire that pin like any other pin.

A reasonable split for this project, matching
[KNOB-PARTS.md](KNOB-PARTS.md): Power, RP2040 Core, Motor Drive, Angle
Sensing, Press Sensing, Buttons, Lighting, and Ambient/Battery Sensing.

This step is optional. A flat single sheet still works electrically. Splitting
only makes a large design easier to read and debug.

---

## 18. Moving from the schematic to the PCB

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

## 19. Drawing the board outline

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

## 20. Placing footprints on the board

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

Open **View → 3D Viewer**, at any point after this step, to see a realistic
render of your board so far. The same SnapEDA export that gave the
TMC6300-LA symbol its footprint also gave it a 3D model. This part should
therefore appear as a real-looking chip, once placed.

---

## 21. Setting default track and via sizes for the board

Do this step once, before you start routing in Section 22. This step sets
the meaning of "the default width of the board," a phrase Section 22 uses.

1. In the PCB Editor, open **File → Board Setup**.
2. Select **Board Stackup → Physical Stackup**, in the list on the left of
   this dialog. Set the copper-layer count here. A two-layer board is
   cheaper, and is enough for a simple design. A four-layer board gives an
   uninterrupted internal ground plane. This plane helps the high-speed
   RP2040 signals, and helps the low-level strain-gauge signal, on this
   design.
   Decide this before you route, since a later change to the layer count
   discards routing work.
3. Select **Design Rules → Constraints**, in the same dialog. Set a minimum
   track width and a minimum clearance here. A common safe starting point,
   for a hobby-scale board, is a 0.15 mm minimum track width, and a 0.15 mm
   minimum clearance. Confirm both figures against the stated capability of
   your chosen fabricator; a fabricator with looser tolerances may need a
   larger minimum.
4. Select **Design Rules → Pre-defined Sizes**, in the same dialog. Add one
   or more named track-width and via-size presets in this list. Add a
   0.25 mm default track as one preset. Add a wider preset too, for the
   motor and battery traces from Section 22.
5. Select **OK** to close the dialog.

The track-width box in the top toolbar, while routing, offers these same
presets in a drop-down list, once this step is complete.

Record your layer-count decision in [DESIGN_NOTES.md](DESIGN_NOTES.md),
alongside the other hardware decisions this project already tracks.

---

## 22. Routing copper traces

1. Press `X`, or select the route-tracks icon.
2. Left-click on a pad, then move your mouse toward the pad the ratsnest
   line points to. KiCad draws a copper trace, following your mouse, snapped
   to the grid.
3. Left-click again, at the destination pad, to finish that trace. The
   ratsnest line for that connection disappears once the trace fully
   connects both ends.
4. Before you route the six motor-phase traces to the TMC6300, and the two
   LiPo power traces, widen the track. Select the wider preset from
   Section 21, in the track-width box in the top toolbar, before you start
   that trace. A wider track carries more current safely. Check the current
   rating in the TMC6300 datasheet, and a PCB trace-width calculator, before
   you confirm an exact width figure. The correct width depends on the
   copper thickness your fabricator uses.
5. Route every remaining signal at the default track width from Section 21,
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
   to see or refresh the filled copper. KiCad does not refill a zone
   automatically after every edit, so repeat this step whenever the copper
   shown on screen looks out of date.

---

## 23. Running the Design Rules Check

The Design Rules Check, DRC for short, is the PCB Editor equivalent of ERC.
DRC catches a physical mistake: two traces placed too close together, a
trace with no connection, or a footprint overlapping the board edge. DRC
checks your board against the exact constraints you set in Section 21, so a
DRC result changes if you revisit that dialog later.

1. Select **Inspect → Design Rules Checker**.
2. Select **Run DRC**.
3. Fix every reported error, the same way you fixed ERC errors: double-click
   a result to jump to that spot, fix it, then run DRC again.

Do not send a board to a fabricator while DRC reports an unresolved error.

---

## 24. Generating the files a fabricator needs

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

## 25. A troubleshooting list, for common first-time issues

- **A pink or red outline, or a "footprint not found" error**: the
  footprint field points at a library nickname your project does not have.
  Revisit Section 10, then Section 14.
- **ERC reports "Pin not driven" on a pin you meant to leave alone**: add a
  no-connect flag, from Section 16.
- **A ratsnest line remains after you routed a trace**: the trace likely
  stopped short of the pad, instead of landing exactly on it. Zoom in, and
  redraw the last segment directly onto the centre of the pad.
- **A pasted symbol appears at an unexpectedly large or small size**: this
  usually means the two schematics disagree on grid or zoom, not that
  anything is actually broken. Press `Ctrl+0` to fit the sheet to the
  screen, then judge the size again; a symbol snapped correctly to the grid
  is the correct size, however it first appeared on screen.
- **The 3D Viewer shows a part in the wrong position, or the wrong
  rotation**: this points to a footprint placed, or rotated, incorrectly on
  the board, not a fault in the 3D model itself. Compare the orientation of
  the footprint, on the 2D board view, against its datasheet pinout
  diagram.
- **You cannot find a menu item exactly as this guide names it**: KiCad
  reorganizes menus slightly between versions. Use the search box inside the
  **Preferences → Hotkeys** table. You can also hover over each toolbar
  icon, to find the closest match to the action this guide describes.
- **You made a mistake and want to start a step over**: press `Ctrl+Z`
  repeatedly. The undo history of KiCad covers most actions in both editors.
- **You made a mistake, already saved it, and even closed KiCad**: this is
  exactly the closing note in Section 8. Run `git diff`, in your working
  copy, to see exactly what changed since your last commit. Run
  `git checkout -- <file>`, naming the affected file, to undo every change
  to that file since your last commit, including the mistake. This only
  helps for a change you already committed at some earlier point; commit
  often, per Section 8, so this safety net stays useful.
- **You are not sure if you saved**: check the title bar of the project
  manager window, and the title bar of each open editor window. Either bar
  shows an asterisk next to the file name, when unsaved changes exist. Press
  `Ctrl+S` in that window to clear it.
- **A part shows correctly for you, but a fresh clone reports it missing**:
  the part most likely comes from a library added under **Global
  Libraries**, on your own computer, instead of **Project Specific
  Libraries**, inside the repository. Revisit Section 6 for the reasoning,
  and Section 10 for the general steps.

---

## 26. How this guide maps to the work already recorded

[DESIGN_NOTES.md](DESIGN_NOTES.md) and [KNOB-PARTS.md](KNOB-PARTS.md) already
record every part this board needs, and every decision still open. Use the
sections of this guide in this order, for a first working schematic:

1. Sections 0 through 8, once, for your project setup, and for getting
   every earlier fix onto your own computer.
2. Section 10, for the SmartKnob footprint libraries you plan to use.
3. Section 11, for the SK6812 LED and the VEML7700 only.
4. Section 12, for every other part: the RP2040 support parts, the
   regulator, the USB-C receptacle, the MAX17048, and each passive part.
5. Section 13 and Section 14, for every symbol already on your sheet, and
   every symbol you add.
6. Section 15, after each group of new symbols.
7. Section 16, until it reports zero errors.
8. Section 17, if the sheet becomes hard to read.
9. Sections 18 through 23, for the physical board.
10. Section 24, only once every earlier section is complete.
