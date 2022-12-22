# elfindo

<img src="graphics/elfindo-logo.svg" width="50%" height="50%" alt="proxperfect logo" align="center"/>

**A parallel find tool for Linux**

elfindo uses multi-threading and a combination of depth and breadth search to discover directory entries faster than the standard Linux GNU `find` utility. It provides basic filters (e.g. for name patterns and timestamps) and an option for JSON output. 

The POSIX `readdir()` function doesn't provide a way to query the entries of a single directory in parallel. Thus, elfindo uses different threads for different directories. Consequently, you will only see a speed advantage over GNU `find` when scanning a path that contains multiple subdirectories.

If you need to run a command for each discovered directory entry, consider piping the output of elfindo to the GNU `xargs` utility, which also supports parallelism (`xargs -P NUM_PROCESSES ...`).

## Usage

The built-in help (`elfindo --help`) provides simple examples to get started.

You can get elfindo pre-built for Linux from the [Releases section](https://github.com/breuner/elfindo/releases).

## Build Prerequisites

Building elfindo requires a C++17 compatible compiler, such as gcc version 8.x or higher.

### Dependencies for Debian/Ubuntu

```bash
sudo apt install build-essential debhelper devscripts fakeroot git lintian
```

### Dependencies for RHEL/CentOS 

```bash
sudo yum install gcc-c++ git make rpm-build
```

#### On RHEL / CentOS 7.x: Prepare Environment with newer gcc Version

Skip these steps on RHEL / CentOS 8.0 or newer.

```bash
sudo yum install centos-release-scl # for CentOS
# ...or alternatively for RHEL: yum-config-manager --enable rhel-server-rhscl-7-rpms
sudo yum install devtoolset-8
scl enable devtoolset-8 bash # alternatively: source /opt/rh/devtoolset-8/enable
```

The `scl enable` command enters a shell in which the environment variables are pointing to a newer gcc version. (The standard gcc version of the system remains unchanged.) Use this shell to run `make` later. The resulting executable can run outside of this shell.

## Build & Install

Start by cloning the main repository:

```bash
git clone https://github.com/breuner/elfindo.git
cd elfindo
```

`make help` will show you all build & install options.

This is the standard build command:

```bash
make -j $(nproc)
```

You can run elfindo directly from the bin subdir (`bin/elfindo`), but you probably want to run `make rpm` or `make deb` now to build a package and install it. On Ubuntu, run this:

```bash
make deb
sudo apt install ./packaging/elfindo*.deb
```

**There you go. Happy finding!**


## Questions & Comments

In case of questions, comments, if something is missing to make elfindo more useful or if you would just like to share your thoughts, feel free to contact me: sven.breuner[at]gmail.com
