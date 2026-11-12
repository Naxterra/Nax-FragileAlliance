# Must never happen

This list is a draft and is waiting for the owner's approval.

- The tools must never modify, move or delete the original game files that the user supplies, including the CD image `FRAGILE.DAT`.
- The repository and any release must never contain game code, graphics, sound, text or data, and must never contain translated output derived from the game.
- The loader must never guess where the application starts inside a bound program. If no MZ stub, or more than one, points to an LE header, it stops with an error.
- The loader must never skip a fixup it does not understand. An unsupported fixup stops the load with a message that names it.
- The translator must never guess a code address. A target it cannot derive is reported as unresolved, and two instructions that overlap are reported as a conflict.
- The runtime must never jump to code that was not translated without stopping with a clear message that names the address.
- The runtime must never leave a half-written save game or settings file. Every write goes to a temporary file that replaces the old file only when it is complete.
- The runtime must never read or write files outside the game folder and its own settings and save folder. A DOS path with ".." never climbs above the root of its drive.
- The runtime must never change or remove an original game file. Writes and removals only touch the user folder.
- Uninstalling must never leave files, registry entries or other system changes behind, apart from save games the user chose to keep.
