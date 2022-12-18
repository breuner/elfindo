# elfindo

<img src="graphics/elfindo-logo.svg" width="50%" height="50%" alt="proxperfect logo" align="center"/>

**A parallel find tool for Linux**

elfindo uses multi-threading and a combination of depth and breadth search to discover directory entries faster than the standard Linux GNU `find` utility. It provides basic filters (e.g. for name patterns and timestamps) and an option for JSON output. 

The POSIX `readdir()` function doesn't provide a way to query the entries of a single directory in parallel. Thus, elfindo uses different threads for different directories. Consequently, you will only see a speed advantage over GNU `find` when scanning a path that contains multiple subdirectories.

If you need to run a command for each discovered directory entry, consider piping the output of elfindo to the GNU `xargs` utility, which also supports parallelism (`xargs -P NUM_PROCESSES ...`).

## Usage

The built-in help (`elfindo --help`) provides simple examples to get started.

You can get elfindo pre-built for Linux from the [Releases section](https://github.com/breuner/elfindo/releases).

## Questions & Comments

In case of questions, comments, if something is missing to make elfindo more useful or if you would just like to share your thoughts, feel free to contact me: sven.breuner[at]gmail.com
