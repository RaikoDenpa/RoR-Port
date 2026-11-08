RULE OF ROSE - unofficial PC port, PLAYTEST BUILD
=================================================

A private test build of a fan-made PC port. Please don't pass it on.
It contains none of the game's files: it runs from YOUR OWN copy of the game,
a disc image (.iso) of the PAL release, SLES-54218.


GETTING STARTED
---------------
1. Unzip the whole folder somewhere you can write to (Desktop, Documents, a
   games drive). Not inside "Program Files".
2. Run RuleOfRose.exe: a settings window opens.
3. Press Play. The first time, it asks for your disc image (.iso). It checks
   it is the PAL SLES-54218 release and copies the game's files into the
   "game" folder (about 2.6 GB, a minute or two, with a progress bar). After
   that Play starts the game straight away.

Windows may warn that the program is unrecognised (it isn't signed): "More
info" > "Run anyway".

Needs Windows 10 or 11 (64-bit), a DirectX 11/12 graphics card and about 3 GB
of free space. The settings window hides while you play and comes back when
the game closes.


SETTINGS (the launcher window)
------------------------------
  Mode              Window or Fullscreen (F11 switches in the game too)
  Window size       1x to 3x the PS2's 640 x 448; the window can also be
                    resized by dragging its edges
  Picture           4:3 as on the PS2, or stretched to fill the window
  Renderer          Automatic is best; try another if the picture is wrong
  Resolution        how sharp the 3D is drawn (2x default; 3x/4x need a
                    stronger graphics card)
  Texture filtering As on the PS2 (default), smooth or sharp
  Skip the intro    start at the title screen
  Play movies       untick to skip every movie
  Volume            the game's sound and the movies
  Controls...       opens pad.ini (keyboard and gamepad mapping)
  Saves / Logs      open those folders
  Disc image...     copy the game's files again from another image


CONTROLS (change them in pad.ini)
---------------------------------
                      Keyboard            Gamepad (Xbox layout)
  Move                W A S D             left stick
  Cross (act/confirm) X or Space          A
  Circle (cancel)     C or Backspace      B
  Triangle            V                   Y
  Square              Z                   X
  Start (item menu)   Enter               Start/Menu
  Select              Tab                 Back/View
  D-pad               arrow keys          d-pad
  L1 / R1             Q / E               LB / RB
  L2 / R2             1 / 3               LT / RT
  Right stick         I J K L             right stick
  Fast-forward (hold) F                   none by default: add one in pad.ini,
                                          e.g. fast_forward = key:F pad:MIDDLE

  Fighting: equip a weapon in the item menu (Action > Equip), hold R1 to raise
  it, press Cross to attack. Enemies grab you: waggle the stick left/right fast.
  Brown: Triangle = Find (after choosing a Find item in the item menu).
  Movies: Start skips them. Other cutscenes can't be skipped (as in the
  original), but holding F runs the game 4x faster, cutscenes included.


SOUND
-----
Music, voices and sound effects all play. If something sounds wrong
(missing, too quiet or too loud, cut off, looping when it shouldn't), please
note where it happened.


SAVES
-----
Save as in the original game. Your memory card is the folder user\memcard
("Saves" in the launcher). The settings are in port.ini and gs.ini next to
RuleOfRose.exe (the launcher writes them).


REPORTING A PROBLEM
-------------------
Every time you play, the launcher makes a new folder user\logs\<date-time>.
When something goes wrong (crash, freeze, the story stops moving, wrong
graphics, wrong sound), please send:
  1. that session's folder from user\logs (zip it),
  2. what you were doing, and the place (the room name shown on screen when you
     enter it),
  3. a screenshot or phone photo if it's visual.
Copying your user\memcard folder now and then also helps: saves from later in
the game let us test those parts directly.

Useful to know as well: does the game feel too fast or too slow? Do the movies
look right (colours) and does their sound stay in sync? Does fighting work on
your controller?
