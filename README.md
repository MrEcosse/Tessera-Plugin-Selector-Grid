# Selector grid

Every option of a selector on screen at once, as a grid of keys. Tessera's own tile for a selector shows the option in
use and opens a list when you tap it; this tile keeps all the options on the page, so one tap picks one. Use it to choose
the input of a TV or amplifier, an activity (Watch TV, Blu-ray, Music), a sound mode, or any list of favourites you keep
in a helper.

It works with anything that has a list: the options come from the entity itself, so there is nothing to type. The
option in use is lit, and follows the entity when it changes anywhere else: on another screen, from a remote, or from an
automation.

When the options do not all fit at a comfortable size, a strip along the bottom of the tile holds an arrow either side of
the page number: tap the left half to go back a page, the right half to go on (from the last page round to the first).
The tile opens on the page of the option in use. A key is never smaller than a finger (about 16 x 9 mm) and the tile
shows at most twelve at once, so a larger tile shows more of them per page. The tile is one cell at least and up to 4x4;
a single cell shows one or two options a page, so 2x1 or 2x2 is where it shines.

## Part of a set for wall panels

This tile belongs to a set of plugins for the same job: media controls that are screwed to a wall instead of held in the
hand. Think of a conference room where the presenter needs the display's input, volume and menu without hunting for a
remote, a sports bar where staff switch each TV to the right match and channel, a restaurant or lounge with background
music and a screen behind the bar, a hotel or holiday-let lounge, a cinema room, or a shop-floor display.

The tiles share one idea. They are **bespoke**: each works with anything Home Assistant can reach, including devices
where the built-in tiles do not fit, and each starts at one cell so it fits any layout.

| Plugin | What it is for |
|---|---|
| [Transport controls](https://github.com/MrEcosse/Tessera-Plugin-Transport-Controls) | skip, stop, play, pause, eject |
| [D-pad](https://github.com/MrEcosse/Tessera-Plugin-D-Pad) | arrows, OK, back, home, menu, guide |
| [Volume controls](https://github.com/MrEcosse/Tessera-Plugin-Volume-Controls) | quieter, mute, louder; or channel down and up |
| [Number pad](https://github.com/MrEcosse/Tessera-Plugin-Number-Pad) | channels, TV or camera numbers (not PIN codes) |
| [Selector grid](https://github.com/MrEcosse/Tessera-Plugin-Selector-Grid) | inputs, activities, sound modes, favourites |

Use them side by side on one panel, or pick the one you need.

## Where the options come from

The tile reads the list from the entity you choose and sends the matching action when a key is tapped:

| Entity | Its options | The one in use | A tap sends |
|---|---|---|---|
| `input_select` (a helper) | `options` | its state | `input_select.select_option option=<the key>` |
| `select` | `options` | its state | `select.select_option option=<the key>` |
| `media_player` | `source_list`, its inputs | `source` | `media_player.select_source source=<the key>` |
| `remote` (a Harmony hub and others) | `activity_list` | `current_activity` | `remote.turn_on activity=<the key>` |

The names on the keys are the options exactly as Home Assistant has them. To use your own words, or a list Home
Assistant does not give (favourites, a set of TV channels, the cameras of a CCTV wall), make an `input_select` helper with
those options and an automation that acts on it when it changes.

## Set up

1. In Tessera, open **Plugins**, choose **Selector grid** and tick your screens. Each screen builds once with the plugin.
2. In Layout, place the **Selector grid** tile from the library's Plugins group, at 2x1 or larger for the most options per
   page.
3. In the inspector, choose the entity: a helper, a select, a media player or a remote.
4. Optionally, under **Leave out**, add the options not to show, written as the selector writes them (`Off`, an input
   you never use). Upper and lower case do not matter.
5. Make sure the screen may perform Home Assistant actions. Tessera turns this on for a screen it added to Home
   Assistant; for a screen added by hand, tick **Allow the device to perform Home Assistant actions** in the ESPHome
   integration's options for that device.

## Example: activities, Harmony-style

Keep the activities of a room in an `input_select` helper (Off, Watch TV, Watch Blu-ray, Music) and let an automation, or
a blueprint such as WHARP's Start Activity, start the activity when the helper changes: turn the devices on, switch the
inputs, set the volume. Point a Selector grid at the helper, leave out `Off` if you have a separate power key, and every
activity is one tap away on the wall. The key of the activity that is running stays lit, whether it was started from the
panel, from a remote or by an automation.

## On the screen

- The option in use is a blue key. A tap lights the key blue while the action goes out, or red when it could not be sent
  (no entity, or the screen may not perform actions).
- Words on a key take up to two lines where the key is high enough, one line where it is not, and end in dots when they
  do not fit.
- The keys go grey while the entity is unavailable. Many TVs give no inputs while they are off; the tile then says there
  are no options to show until the TV is on.
- Works on every board, the CYD included. Where a tile is small, it shows fewer options a page; give it more room.

## Good to know

- The tile sends only the four actions above, each with an option's own name, to the entity you chose.
- It keeps up to 64 options. A tile's data from Home Assistant is at most about 2.6 KB, so a very long list (a streaming
  box with a hundred apps) is shortened from the end before that; leave options out, or use a helper with the ones you
  want.
- No swiping or scrolling: on a Tessera screen a swipe changes the page of the screen, so the tile pages with taps.
- Nothing leaves your home: the tile uses no internet service and no Home Assistant entity but the one you choose.

## How it works

The manifest names the entity's list attributes (`options`, `source_list`, `source`, `activity_list`,
`current_activity`), so the tile gets them with every change of the entity. It keeps the options in the screen's PSRAM
where the board has it, rebuilds its keys only when the list itself changes, and otherwise repaints the lit key. A tap
finds the key from where the finger was (`tessera::tap_point()`) and sends the domain's action with
`tessera::action`.
