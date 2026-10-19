Put your own Tekken Tag Tournament 2 (Xbox 360) title update and DLC packages here, then run
Setup.bat (or "Rebuild the game" in the launcher if the game is already set up).

  - Title update: files named like  TU_174Q22P_0000008000000.00000000000O2  or
    tu00000005_00000000, from the console's Cache folder or from
    Content\0000000000000000\4E4D0859\000B0000\ on its hard drive.
    Title Update 3 (the newest, it includes the earlier ones) is applied; older ones are ignored.
  - DLC: the package files (long hexadecimal names) from
    Content\0000000000000000\4E4D0859\00000002\

Copy the files as they are; don't rename or extract them. Subfolders are fine. Setup checks
every package against its own hashes and only uses Xbox 360 packages for this game (title ID
4E4D0859); anything else in here is ignored.

The DLC characters need their packages: besides unlocking them, each package carries a data
archive the game loads, which is not on the disc.

Nothing in this folder is ever uploaded: .gitignore excludes it.
