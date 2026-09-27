ESP32 MEDIA REMOTE - WINDOWS
============================

This is the PC half of the touch remote. The firmware itself already lives in
the board's flash, along with your accent colour and coin choice -- those
travel with the device, not with the machine.


GETTING STARTED
---------------

1. Plug the remote into a USB port with data lines.
   (A charge-only cable gives it power but no connection.)

2. Double-click Start.bat to try it straight away.
   A window opens and shows what is happening. Close it to stop.

3. Once you are happy: double-click Install.bat.
   That copies the program to your local disk and starts it automatically
   every time you log in, without a window.

Undo with Uninstall.bat.

Nothing needs installing -- no Python, no dependencies. Everything required is
in this folder, and none of it needs administrator rights.


WHAT WORKS
----------

Previous, play/pause and next go to whatever is playing. Where the player has
another track, the buttons change track; where it does not -- a single video in
a browser tab -- they move ten seconds instead. Both go through the Windows
media session, the same one behind the playback box that appears when you press
a volume key, so the remote knows which of the two it did.

Title and artist come from that same session.

Volume and mute go through the Windows mixer, so the slider on the board sets a
real level and reads it back.

Crypto prices are fetched from CoinGecko by this PC and sent to the board over
the USB cable. The board has no network connection of its own.


IF SOMETHING DOES NOT WORK
--------------------------

Run Troubleshoot.bat. It first lists every serial port Windows can see and
marks the remote if it is there. Then it runs the program with full logging, so
you can watch each command arrive.

If it cannot find the board, it is usually the cable. Plenty of USB-C cables
are charge-only, with no data lines.

Buttons but no title: the Windows media session is unavailable. The buttons
still work.

The volume field shows "--": the mixer could not be read. The slider falls back
to nudging the volume keys, so it still works, but the level cannot be shown.

Nothing happens at all, and Troubleshoot.bat says "Access denied" when it
starts python.exe: you are running it from a network share that does not grant
execute permission. Run Install.bat instead -- it copies to local disk first --
or copy the folder to your own disk and start it from there.


THIS FOLDER
-----------

Start.bat          Run in a visible window (to try it)
Install.bat        Copy locally and start automatically at login
Uninstall.bat      Remove the autostart entry and the local copy
Troubleshoot.bat   Show serial ports and run with full logging

host\              The program itself
python\            An embedded CPython -- no installation needed
lib\               The libraries it uses


SOURCE
------

The full project, firmware included, is at:

  https://github.com/Chizeeey/esp32-c6-touch-media-remote

This bundle is MIT licensed; see LICENSE. The bundled interpreter and libraries
keep their own licences, listed in THIRD-PARTY-NOTICES.md.
