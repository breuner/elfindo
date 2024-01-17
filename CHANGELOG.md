# Changelog of elfindo

## v0.9.5 (work in progress)

### New Features & Enhancements
* New option "--newer PATH" to filter based on mtime of given path.
* New option "--exec" to execute arbritrary system commands for each discovered file/dir.

### Contributors
* Thanks to Ido Szargel for helpful comments and suggestions.

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
