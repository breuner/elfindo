# Changelog of elfindo

## v1.0.3 (Sep 24, 2024)

### New Features & Enhancements
* New options to filter by user and group. (See `--uid`, `--user`, `--gid`, `--group`.)
* New options to not descend directories on other filesystems. (See `--xdev`, `--mount`.)

### Contributors
* Thanks to Anna Fuchs for helpful comments and suggestions.

## v1.0.1 (Apr 14, 2024)

### New Features & Enhancements
* New option "--newer PATH" to filter based on mtime of given path.
* New option "--exec" to execute arbritrary system commands for each discovered file/dir.
* New option "--quit" to quit after the first match was found.

### General Changes
* Static builds now based on latest Alpine Linux 3.x instead of always 3.14.
* Names of discovered files/dirs now get printed before other processing, such as copy or unlink.
* Option "--name" also filters directory names now.

### Contributors
* Thanks to Ido Szargel, Adar Zinger, Deborah Gironde for helpful comments and suggestions.

### Fixes
* Fixed potential file descriptor leak when reading dir contents fails.

## v0.9.4 (Nov 11, 2023)

### New Features & Enhancements
* Option "--name PATTERN" may now be given multiple times to match any of the given patterns.

### Contributors
* Thanks to Ido Szargel for helpful comments and suggestions.

## v0.9.2 (Dec 27, 2022)

### New Features & Enhancements
* New option "--copyto PATH" to copy all discovered regular files, symlinks, dirs from source to given destination.
* New option "--unlink" to delete all files under the given path.

### Contributors
* Thanks to Vang Le Quy for helpful comments and suggestions.

## v0.9 (Dec 18, 2022)
* Initial release by Sven Breuner.

### Contributors
* Thanks to Paul Hargreaves for helpful comments and suggestions.
