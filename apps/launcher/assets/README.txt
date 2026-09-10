HT2MP launcher profile asset
============================

HT2MP.pl1 is the verified exact-Steam driver template used by release builds.
It was deliberately created in game as the clean `online_template` save and
then imported into this directory; the build never reads a developer's live
Steam profile.

Canonical SHA-256:
F25D4DC627B366C50CA30615A0DF46B43FA4E4B20D94DEA7658199A72A8DBA34

SHA-256: F25D4DC627B366C50CA30615A0DF46B43FA4E4B20D94DEA7658199A72A8DBA34

Required template properties:
- Windows Structured Storage driver format ver=4 / settings schema=5;
- one anonymous driver;
- exactly one completed listofgames slot containing XAI, env, anm, stat, cont;
- no player-identifying names or historical logs.

The `launcher-bundled-template` test opens the real asset through Windows
Structured Storage, checks the exact schema and one-slot invariant, prepares a
private online copy, reopens it for verification, and proves that this source
asset was not changed.

To refresh the asset, build `ht2mp_profile_template_tool` and run:

  ht2mp-profile-template-tool --prune-copy <source.pl1> <asset.pl1> online_template

The exact Steam game rejects a compound file reconstructed with IStorage::CopyTo.
The prune-copy operation therefore preserves the game's original compound-file
layout and transactionally removes every slot except `online_template`. Verify
the result with --list and scan it for names belonging to removed saves before
committing a refreshed asset.

At runtime the launcher copies this file into the isolated stage and never
modifies the distribution asset.  A source-install .pl1 is accepted only as a
local development fallback and is likewise copied rather than edited.

The template's native driver identity is `ВОДИТЕЛЬ`. The isolated runtime copy
is consequently named `ВОДИТЕЛЬ.pl1`, and staged TRUCK.INI is written as CP1251
`lastp=ВОДИТЕЛЬ`. Renaming only the file to HT2MP.pl1 makes the stock Load action
return to the main menu because it no longer matches the embedded identity.
