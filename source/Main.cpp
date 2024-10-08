/**
 * Parallel search for files & dirs in a directory hierarchy.
 *
 * This prog uses a combination of breadth and depth search. Breadth to generate parallelism, depth
 * to limit memory usage.
 *
 * (If only one thread is active, then it does depth search, because there is no parallelism
 * anyways.)
 */

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fnmatch.h>
#include <getopt.h>
#include <grp.h>
#include <inttypes.h> // defines PRIu64 for printf uint64_t
#include <iostream>
#include <iomanip>
#include <libgen.h>
#include <list>
#include <mutex>
#include <pwd.h>
#include <signal.h>
#include <stack>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <time.h>
#include <thread>
#include <unistd.h>
#include <vector>


#define ARG_FILTER_ATIME	"atime"
#define ARG_ACLCHECK_LONG	"aclcheck"
#define ARG_COPYDEST_LONG	"copyto"
#define ARG_FILTER_CTIME	"ctime"
#define ARG_EXEC_LONG		"exec"
#define ARG_GID_LONG		"gid"
#define ARG_GODEEP_LONG		"godeep"
#define ARG_GROUP_LONG		"group"
#define ARG_HELP_SHORT		'h'
#define ARG_HELP_LONG		"help"
#define ARG_JSON_LONG		"json"
#define ARG_MAXDEPTH_LONG	"maxdepth"
#define ARG_MOUNT_LONG		"mount"
#define ARG_FILTER_MTIME	"mtime"
#define ARG_NAME_LONG		"name"
#define ARG_NEWER_LONG		"newer"
#define ARG_NOCOPYERR_LONG	"nocopyerr"
#define ARG_NODELERR_LONG	"nodelerr"
#define ARG_NOPRINT_LONG	"noprint"
#define ARG_NOSUMMARY_LONG	"nosummary"
#define ARG_NOTIMEUPD_LONG	"notimeupd"
#define ARG_PATH_LONG		"path"
#define ARG_PRINT0_LONG		"print0"
#define ARG_QUITAFTER1_LONG "quit"
#define ARG_FILTER_SIZE		"size"
#define ARG_STAT_LONG		"stat"
#define ARG_THREADS_SHORT	't'
#define ARG_THREADS_LONG	"threads"
#define ARG_SEARCHTYPE_LONG	"type"
#define ARG_UID_LONG		"uid"
#define ARG_UNLINK_LONG		"unlink"
#define ARG_USER_LONG		"user"
#define ARG_VERBOSE_LONG	"verbose"
#define ARG_VERSION_LONG	"version"
#define ARG_XDEV_LONG		"xdev"

#define DIRENTRY_JSON_TYPE_BLK		"blockdev"
#define DIRENTRY_JSON_TYPE_CHR		"chardev"
#define DIRENTRY_JSON_TYPE_DIR		"dir"
#define DIRENTRY_JSON_TYPE_FIFO		"fifo"
#define DIRENTRY_JSON_TYPE_LNK		"symlink"
#define DIRENTRY_JSON_TYPE_REG		"regfile"
#define DIRENTRY_JSON_TYPE_SOCK		"unixsock"
#define DIRENTRY_JSON_TYPE_UNKNOWN	"unknown"

// flags for filterSizeAndTimeFlags
#define FILTER_FLAG_SIZE_EXACT		(1 << 0)
#define FILTER_FLAG_SIZE_LESS		(1 << 1)
#define FILTER_FLAG_SIZE_GREATER	(1 << 2)
#define FILTER_FLAG_MTIME_EXACT		(1 << 3)
#define FILTER_FLAG_MTIME_LESS		(1 << 4)
#define FILTER_FLAG_MTIME_GREATER	(1 << 5)
#define FILTER_FLAG_CTIME_EXACT		(1 << 6)
#define FILTER_FLAG_CTIME_LESS		(1 << 7)
#define FILTER_FLAG_CTIME_GREATER	(1 << 8)
#define FILTER_FLAG_ATIME_EXACT		(1 << 9)
#define FILTER_FLAG_ATIME_LESS		(1 << 10)
#define FILTER_FLAG_ATIME_GREATER	(1 << 11)

#define EXEC_ARG_PATH_PLACEHOLDER	"{}"
#define EXEC_ARG_TERMINATOR			";"

typedef std::vector<std::string> StringVec;

// short-hand macro to either return or exit on fatal errors depending on user config
#define EXIT_OR_RETURN_CONFIGURABLE(ignoreError)	{ if(ignoreError) return; else exit(1); }

struct ExternalProgExec
{
	StringVec cmdLineStrVec; // cmd and args if exec given by user, one of them being {} for path
};

struct Config
{
	unsigned numThreads {16};
	unsigned depthSearchStartThreshold {0}; // start depth search when this num of dirs is in stack
	bool printSummary {true}; // print scan summary at the end
	bool printVerbose {false}; // true to enable verbose output
	bool printVersion {false}; // print version and exit
	bool statAll {false}; // true to call stat() on all discovered entries
	bool checkACLs {false}; // true to query ACLs on all discovered entries
	bool printJSON {false}; // true to print output in JSON format. (each entry is one JSON object)
	unsigned short maxDirDepth { (unsigned short)~0}; // max dir depth to scan. (args have depth 0)
	std::list<std::string> scanPaths; // user-provided paths to scan
	char searchType {0}; // search type. 0=all, 'f'=reg_files, 'd'=dirs.
	bool print0 {false}; // whether to terminate entry names with '\0' instead '\n'
	StringVec nameFilterVec; // or-filter on multiple filenames (in contrast to full path)
	std::string pathFilter; // filter on full path
	struct
	{
		uint64_t sizeExact {0}, sizeLess {0}, sizeGreater {0};
		uint64_t mtimeExact {0}, mtimeLess {0}, mtimeGreater {0};
		uint64_t ctimeExact {0}, ctimeLess {0}, ctimeGreater {0};
		uint64_t atimeExact {0}, atimeLess {0}, atimeGreater {0};

		unsigned filterSizeAndTimeFlags; // FILTER_FLAG_..._{EXACT,LESS,GREATER} flags
	} filterSizeAndTime;
	uint64_t filterUID {~0ULL}; // numeric user ID
	uint64_t filterGID {~0ULL}; // numeric group ID
	uint64_t filterMountID {~0ULL}; // stay on mountpoint
	std::string copyDestDir; // target dir for file/dir copies
	bool ignoreCopyErrors {false}; // ignore copy errors
	bool printEntriesDisabled {false}; // true to disable print of discovered entries
	bool unlinkFiles {false}; // true to unlink all discovered files (not dirs)
	bool ignoreUnlinkErrors {false}; // ignore unlink errors
	bool copyTimeUpdate {true}; // update atime/mtime when copying files
	ExternalProgExec exec; // config to execute external prog for each disovered entry
	bool quitAfterFirstMatch {false}; // true to quit after first match
} config;

struct State
{
	std::chrono::steady_clock::time_point startTime {std::chrono::steady_clock::now()};

	std::stack<std::thread> scanThreads;
} state;

struct Statistics
{
	std::atomic_uint64_t numDirsFound {0};
	std::atomic_uint64_t numFilesFound {0};
	std::atomic_uint64_t numUnknownFound {0};
	std::atomic_uint64_t numFilterMatches {0};
	std::atomic_uint64_t numStatCalls {0};
	std::atomic_uint64_t numAccessACLsFound {0};
	std::atomic_uint64_t numDefaultACLsFound {0};
	std::atomic_uint64_t numErrors {0}; // e.g. permission errors
	std::atomic_uint64_t numBytesCopied {0};
	std::atomic_uint64_t numFilesNotCopied {0}; // num skipped because non-regular file type
} statistics;

class ScanDoneException : public std::exception {};

/**
 * This is the stack for directories that were found by the breadth search threads.
 */
class SharedStack
{
	private:
		struct StackElem
		{
			StackElem(const std::string& dirPath, unsigned short dirDepth) :
				dirPath(dirPath), dirDepth(dirDepth) {}

			std::string dirPath;
			unsigned short dirDepth; // dirPath depth relative to start path
		};

	public:
		SharedStack() {}

	private:
		std::stack<StackElem> dirPathStack;
		std::mutex mutex;
		std::condition_variable condition; // when new elems are pushed
		unsigned numWaiters {0}; // detect termination when equal to number of threads
		std::atomic_uint64_t stackSize {0}; // to get stack size lock-free

	public:
		void push(const std::string& dirPath, unsigned short dirDepth)
		{
			std::unique_lock<std::mutex> lock(mutex); // L O C K

			dirPathStack.push(StackElem(dirPath, dirDepth) );

			stackSize++;

			condition.notify_one();
		}

		/**
		 * If stack is empty, this waits for a new push.
		 *
		 * @return false if stack was empty, so outDirPath did not get assigned.
		 * @throw ScanDoneException when all threads were waiting, so no thread was active anymore
		 * 		to add more dirs to the queue.
		 */
		bool popWait(std::string& outDirPath, unsigned short& outDirDepth)
		{
			std::unique_lock<std::mutex> lock(mutex); // L O C K

			numWaiters++;

			while(dirPathStack.empty() )
			{
				if(numWaiters == config.numThreads)
				{ // all threads waiting => end of dir tree scan
					// note: no numWaiters-- here, so that all threads see termination condition
					condition.notify_all();
					throw ScanDoneException();
				}

				condition.wait(lock);
			}

			numWaiters--;

			if(dirPathStack.empty() )
				return false;

			StackElem& topElem = dirPathStack.top();
			outDirPath = topElem.dirPath;
			outDirDepth = topElem.dirDepth;

			dirPathStack.pop();

			stackSize--;

			return true;
		}

		/**
		 * @return false if stack was empty, so outDirPath did not get assigned.
		 */
		bool pop(std::string& outDirPath, unsigned short& outDirDepth)
		{
			std::unique_lock<std::mutex> lock(mutex); // L O C K

			if(dirPathStack.empty() )
				return false;

			StackElem& topElem = dirPathStack.top();
			outDirPath = topElem.dirPath;
			outDirDepth = topElem.dirDepth;

			dirPathStack.pop();

			stackSize--;

			return true;
		}

		/**
		 * Lock-free getter of current stack size.
		 */
		uint64_t getSize()
		{
			return stackSize;
		}

} sharedStack;

/**
 * Check ACL of given file or dir.
 *
 * @path: path to entry for which ACLs should be checked.
 * @isDirectory: true of given entry is a directory.
 */
void checkACLs(const char* path, bool isDirectory)
{
	if(!config.checkACLs)
		return; // nothing to do

	ssize_t getAccessRes = lgetxattr(path, "system.posix_acl_access", NULL, 0);

	if(getAccessRes >= 0)
		statistics.numAccessACLsFound++;
	else
	if( (errno != ENODATA) && (errno != ENOTSUP) )
		fprintf(stderr, "Failed to get Access ACL for entry: %s; Error: %s\n",
			path, strerror(errno) );

	// dirs have additional default ACL check
	if(isDirectory)
	{
		ssize_t getDefaultRes = lgetxattr(path, "system.posix_acl_default", NULL, 0);

		if(getDefaultRes >= 0)
			statistics.numAccessACLsFound++;
		else
		if( (errno != ENODATA) && (errno != ENOTSUP) )
			fprintf(stderr, "Failed to get Default ACL for dir: %s; Error: %s\n",
				path, strerror(errno) );
	}
}

/**
 * Add escape characters to make a string usable in JSON.
 *
 * @string input string to convert
 * @return copy of input string with escaped
 */
std::string escapeStrforJSON(const std::string &string)
{
	std::ostringstream stringStream;

	for (const auto& currentChar : string)
	{
		/* the case statments are the typical shortcut escapes, the "default" statment handles
			generic escapes */
		switch (currentChar)
		{
			case '"':
				stringStream << "\\\"";
			break;
			case '\\':
				stringStream << "\\\\";
			break;
			case '\b':
				stringStream << "\\b";
			break;
			case '\f':
				stringStream << "\\f";
			break;
			case '\n':
				stringStream << "\\n";
			break;
			case '\r':
				stringStream << "\\r";
			break;
			case '\t':
				stringStream << "\\t";
			break;

			default:
				if('\x00' <= currentChar && currentChar <= '\x1f')
				{ // escape this character
					stringStream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
							<< static_cast<int>(currentChar);
				}
				else
				{ // nothing to escape
					stringStream << currentChar;
				}
		}
	}

	return stringStream.str();
}

/**
 * Filter printed entries by user-defined entry type.
 *
 * If entry type cannot be determined and a user-defined filter is set then this returns false.
 *
 * @return true if entry passes the filter and should be printed, false otherwise.
 */
bool filterPrintEntryByType(const std::string& entryPath, const struct dirent* dirEntry,
	const struct stat* statBuf)
{
	if(!config.searchType)
		return true; // no filter defined by user => always passes

	if(dirEntry && (dirEntry->d_type != DT_UNKNOWN) )
	{
		char matchType = 0; // values taken from "man find(1)"

		switch(dirEntry->d_type)
		{
			case DT_BLK: 	matchType = 'b'; break;
			case DT_CHR: 	matchType = 'c'; break;
			case DT_DIR: 	matchType = 'd'; break;
			case DT_FIFO: 	matchType = 'p'; break;
			case DT_LNK: 	matchType = 'l'; break;
			case DT_REG: 	matchType = 'f'; break;
			case DT_SOCK: 	matchType = 's'; break;
		}

		if(matchType == config.searchType)
			return true;

		return false;
	}

	if(statBuf)
	{
		char matchType = 0; // values taken from "man find(1)"

		if(S_ISBLK(statBuf->st_mode) )
			matchType = 'b';
		else
		if(S_ISCHR(statBuf->st_mode) )
			matchType = 'c';
		else
		if(S_ISDIR(statBuf->st_mode) )
			matchType = 'd';
		else
		if(S_ISFIFO(statBuf->st_mode) )
			matchType = 'p';
		else
		if(S_ISLNK(statBuf->st_mode) )
			matchType = 'l';
		else
		if(S_ISREG(statBuf->st_mode) )
			matchType = 'f';
		else
		if(S_ISSOCK(statBuf->st_mode) )
			matchType = 's';

		if(matchType == config.searchType)
			return true;

		return false;
	}

	// we have neither d_type nor statBuf and a filter has been definded
	fprintf(stderr, "Cannot identify type of entry. Path: %s\n", entryPath.c_str() );

	return false;
}

/**
 * Filter printed files by user-defined filename pattern.
 *
 * If entry type cannot be determined and a user-defined filter is set then this returns false.
 *
 * @return true if entry passes the filter and should be printed, false otherwise.
 */
bool filterPrintEntryByName(const std::string& entryPath, const struct dirent* dirEntry,
	const struct stat* statBuf)
{
	if(config.nameFilterVec.empty() )
		return true; // no filter defined by user => always passes

	// check whether any of the given filter names matches

	std::string currentFilename = std::filesystem::path(entryPath).filename().string();

	for(std::string& nameFilter : config.nameFilterVec)
	{
		int matchRes = fnmatch(nameFilter.c_str(), currentFilename.c_str(), 0 /* flags */);

		if(!matchRes)
			return true; // we have a match
	}

	return false;
}

/**
 * Filter printed files by user-defined path pattern.
 *
 * If entry type cannot be determined and a user-defined filter is set then this returns false.
 *
 * @return true if entry passes the filter and should be printed, false otherwise.
 */
bool filterPrintEntryByPath(const std::string& entryPath, const struct dirent* dirEntry,
	const struct stat* statBuf)
{
	if(config.pathFilter.empty() )
		return true; // no filter defined by user => always passes

	bool isFile =
		(dirEntry && (dirEntry->d_type != DT_UNKNOWN) && (dirEntry->d_type != DT_DIR) ) ||
		(statBuf && !S_ISDIR(statBuf->st_mode) );

	if(!isFile)
		return false; // anything that's not a file can't match

	int matchRes = fnmatch(config.pathFilter.c_str(), entryPath.c_str(), 0 /* flags */);

	if(!matchRes)
		return true; // we have a match

	return false;
}

/**
 * Check for size or timestamp exact/less/greater match.
 * This returns false if a filter is defined that doesn't match. Otherwise it will run through
 * without calling "return".
 * Note that there can be more than one filter defined, so this can't return true on first match.
 * Example: CHECK_EXACT_LESS_GREATER_VAL(atime, ATIME, atim.tv_sec)
 */
#define _CHECK_EXACT_LESS_GREATER_VAL(lowercaseCfgName, uppercaseCfgName, statBufField) \
		if(config.filterSizeAndTime.filterSizeAndTimeFlags & FILTER_FLAG_ ## uppercaseCfgName ## _EXACT) \
			if( (uint64_t)statBuf->st_ ## statBufField != config.filterSizeAndTime.lowercaseCfgName ## Exact) \
				return false; \
		if(config.filterSizeAndTime.filterSizeAndTimeFlags & FILTER_FLAG_ ## uppercaseCfgName ## _LESS) \
			if( (uint64_t)statBuf->st_ ## statBufField >= config.filterSizeAndTime.lowercaseCfgName ## Less) \
				return false; \
		if(config.filterSizeAndTime.filterSizeAndTimeFlags & FILTER_FLAG_ ## uppercaseCfgName ## _GREATER) \
			if( (uint64_t)statBuf->st_ ## statBufField <= config.filterSizeAndTime.lowercaseCfgName ## Greater) \
				return false;
#define CHECK_EXACT_LESS_GREATER_VAL(lowercaseCfgName, uppercaseCfgName, statBufField) \
		_CHECK_EXACT_LESS_GREATER_VAL(lowercaseCfgName, uppercaseCfgName, statBufField)


/**
 * Filter printed files by user-defined size or timestamp.
 *
 * This requires stat() info.
 *
 * If entry type cannot be determined and a user-defined filter is set then this returns false.
 *
 * @return true if entry passes the filter and should be printed, false otherwise.
 */
bool filterPrintEntryBySizeOrTime(const std::string& entryPath, const struct dirent* dirEntry,
	const struct stat* statBuf)
{
	if(!config.filterSizeAndTime.filterSizeAndTimeFlags)
		return true; // no filter defined by user => always passes

	bool isFile =
		(dirEntry && (dirEntry->d_type != DT_UNKNOWN) && (dirEntry->d_type != DT_DIR) ) ||
		(statBuf && !S_ISDIR(statBuf->st_mode) );

	if(!isFile)
		return false; // anything that's not a file can't match

	if(!statBuf)
		return false; // can't filter without stat() info

	// the macro will return false if a defined filter doesn't match, otherwise it runs through.
	CHECK_EXACT_LESS_GREATER_VAL(size, SIZE, size);
	CHECK_EXACT_LESS_GREATER_VAL(atime, ATIME, atim.tv_sec);
	CHECK_EXACT_LESS_GREATER_VAL(ctime, CTIME, ctim.tv_sec);
	CHECK_EXACT_LESS_GREATER_VAL(mtime, MTIME, mtim.tv_sec);

	return true; // all filters passed
}

/**
 * Filter printed files by user-defined UID and GID.
 *
 * This requires stat() info.
 *
 * @return true if entry passes the filter and should be printed, false otherwise.
 */
bool filterPrintEntryByUIDAndGID(const std::string& entryPath, const struct dirent* dirEntry,
	const struct stat* statBuf)
{
	// filter UID
	if(config.filterUID != ~0ULL)
	{
		if(!statBuf || (statBuf->st_uid != config.filterUID) )
			return false; // either no statBuf or UID mismatch
	}

	// filter GID
	if(config.filterGID != ~0ULL)
	{
		if(!statBuf || (statBuf->st_gid != config.filterGID) )
			return false; // either no statBuf or GID mismatch
	}

	return true;
}


/**
 * Replace all occurrences of EXEC_ARG_PATH_PLACEHOLDER in "subject" string with the given "path"
 * string.
 */
void replacePathPlaceholerWithPath(std::string& subject, const std::string& path)
{
    size_t pos = 0;

    while( (pos = subject.find(EXEC_ARG_PATH_PLACEHOLDER, pos) ) != std::string::npos)
    {
         subject.replace(pos, strlen(EXEC_ARG_PATH_PLACEHOLDER), path);
         pos += path.length();
    }
}

/**
 * Execute user-given system command for discovered entry.
 */
void execSystemCommand(const std::string& entryPath)
{
	if(config.exec.cmdLineStrVec.empty() )
		return; // nothing to do

	std::string commandStr;

	// add executable
	commandStr.append("'");
	commandStr.append(config.exec.cmdLineStrVec[0] );
	commandStr.append("' ");

	// add args and replace placeholder with path
	for(size_t i=1; i < config.exec.cmdLineStrVec.size(); i++)
	{
		std::string argStr(config.exec.cmdLineStrVec[i] );

		replacePathPlaceholerWithPath(argStr, entryPath);

		commandStr.append("'");
		commandStr.append(argStr);
		commandStr.append("' ");
	}

	// flush is necessary for cases where stdout is not line-buffered, e.g. because it's not a tty.
	fflush(stdout);

	int sysRes = std::system(commandStr.c_str() );
	if(WIFSIGNALED(sysRes) )
	{
		fprintf(stderr, "Aborting because exec command terminated on signal. "
			"Signal: %d; Path: %s\n", (int)WTERMSIG(sysRes), entryPath.c_str() );

		// note: we really need SIGTERM here, as SIGINT does not reliably kill running system cmds
		kill(0, SIGTERM);
	}
}

/**
 * Copy entry if it's a regular file, dir or symlink; skip others.
 * This won't preserve hardlinks.
 */
void copyEntry(const std::string& entryPath, const struct dirent* dirEntry,
	const struct stat* statBuf)
{
	if(config.copyDestDir.empty() )
		return;

	std::string relativeEntryPath = entryPath.substr(config.scanPaths.front().length() );
	std::string destPath = config.copyDestDir + "/" + relativeEntryPath;

	if(config.printVerbose)
		fprintf(stderr, "Copying: %s -> %s\n", entryPath.c_str(), destPath.c_str() );

	// config.statAll is forced to true when config.copyDestDir is set

	if(S_ISDIR(statBuf->st_mode) )
	{ // create directory
		int mkRes = mkdir(destPath.c_str(),
				(statBuf->st_mode & 0777) | ( S_IRUSR | S_IWUSR | S_IXUSR) ); // user always rwx
		if( (mkRes == -1) && (errno != EEXIST) )
		{
			fprintf(stderr, "Failed to create dir: %s; Error: %s\n",
				destPath.c_str(), strerror(errno) );

			statistics.numErrors++;

			EXIT_OR_RETURN_CONFIGURABLE(config.ignoreCopyErrors);
		}

		if(config.copyTimeUpdate)
		{ // update timestamps
			struct timespec newTimes[2] = {statBuf->st_atim, statBuf->st_mtim};

			int updateTimeRes = utimensat(AT_FDCWD, destPath.c_str(), newTimes, 0);
			if(updateTimeRes == -1)
			{
				fprintf(stderr, "Failed to update timestamps of copy destination dir: %s; "
					"Error: %s\n", destPath.c_str(), strerror(errno) );

				statistics.numErrors++;
			}
		}
	}
	else
	if(S_ISLNK(statBuf->st_mode) )
	{ // copy symlink
		const unsigned bufSize = 16*1024; // 16KiB copy buffer
		char* buf = (char*)malloc(bufSize);

		if(!buf)
		{
			fprintf(stderr, "Failed to allocate memory buffer for symlink copy. Alloc size: %u\n",
				bufSize);

			exit(EXIT_FAILURE);
		}

		ssize_t readRes = readlink(entryPath.c_str(), buf, bufSize);
		if(readRes == bufSize)
		{
			fprintf(stderr, "Failed to copy symlink due to long target path: %s; Max: %u\n",
				entryPath.c_str(), bufSize);

			statistics.numErrors++;
			free(buf);

			EXIT_OR_RETURN_CONFIGURABLE(config.ignoreCopyErrors);
		}
		else
		if(readRes == -1)
		{
			fprintf(stderr, "Failed to read symlink for copying: %s; Error: %s\n",
				entryPath.c_str(), strerror(errno) );

			statistics.numErrors++;
			free(buf);

			EXIT_OR_RETURN_CONFIGURABLE(config.ignoreCopyErrors);
		}

		// readlink() does not zero-terminate the string in buf
		buf[readRes] = 0;

		int linkRes = symlink(buf, destPath.c_str() );
		if( (linkRes == -1) && (errno == EEXIST) )
		{ // symlink() can't overwrite existing file, so unlink and try again
			unlink(destPath.c_str() );

			linkRes = symlink(buf, destPath.c_str() );
		}

		if(linkRes == -1)
		{
			fprintf(stderr, "Failed to create symlink for copying: %s; Error: %s\n",
				destPath.c_str(), strerror(errno) );

			statistics.numErrors++;
			free(buf);

			EXIT_OR_RETURN_CONFIGURABLE(config.ignoreCopyErrors);
		}

		if(config.copyTimeUpdate)
		{ // update timestamps
			struct timespec newTimes[2] = {statBuf->st_atim, statBuf->st_mtim};

			int updateTimeRes = utimensat(AT_FDCWD, destPath.c_str(), newTimes, AT_SYMLINK_NOFOLLOW);
			if(updateTimeRes == -1)
			{
				fprintf(stderr, "Failed to update timestamps of copy destination symlink: %s; "
					"Error: %s\n", destPath.c_str(), strerror(errno) );

				statistics.numErrors++;
			}
		}

		// symlink copy done => cleanup
		free(buf);
	}
	else
	if(S_ISREG(statBuf->st_mode) )
	{ // copy regular file
		// (no atime update simiar to "cp -a" behavior)
		int sourceFD = open(entryPath.c_str(), O_RDONLY | O_NOATIME);
		if(sourceFD == -1)
		{
			fprintf(stderr, "Failed to open copy source file for reading: %s; Error: %s\n",
				entryPath.c_str(), strerror(errno) );

			statistics.numErrors++;

			EXIT_OR_RETURN_CONFIGURABLE(config.ignoreCopyErrors);
		}

		int destFD = open(destPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY,
			(statBuf->st_mode & 0777) | ( S_IRUSR | S_IWUSR) ); // user/owner can always read+write
		if(destFD == -1)
		{
			fprintf(stderr, "Failed to open copy destination file for writing: %s; Error: %s\n",
				destPath.c_str(), strerror(errno) );

			statistics.numErrors++;
			close(sourceFD);

			EXIT_OR_RETURN_CONFIGURABLE(config.ignoreCopyErrors);
		}

		const unsigned bufSize = 4*1024*1024; // 4MiB copy buffer
		char* buf = (char*)malloc(bufSize);
		ssize_t readRes;

		if(!buf)
		{
			fprintf(stderr, "Failed to allocate memory buffer for file copy. Alloc size: %u\n",
				bufSize);

			exit(EXIT_FAILURE);
		}

		// copy file contents
		for( ; ; )
		{
			readRes = read(sourceFD, buf, bufSize);
			if(!readRes)
				break;
			else
			if(readRes == -1)
			{
				fprintf(stderr, "Failed to read from copy source file: %s; Error: %s\n",
					entryPath.c_str(), strerror(errno) );

				statistics.numErrors++;
				close(sourceFD);
				close(destFD);
				free(buf);

				EXIT_OR_RETURN_CONFIGURABLE(config.ignoreCopyErrors);
			}

			ssize_t writeRes = write(destFD, buf, readRes);
			if(writeRes == -1)
			{
				fprintf(stderr, "Failed to write to copy destination file: %s; Error: %s\n",
					destPath.c_str(), strerror(errno) );

				statistics.numErrors++;
				close(sourceFD);
				close(destFD);
				free(buf);

				EXIT_OR_RETURN_CONFIGURABLE(config.ignoreCopyErrors);
			}
			else
			if(readRes != writeRes)
			{
				fprintf(stderr, "Failed to write to copy destination file: %s; "
					"Expected write size: %zd; Actual write size: %zd\n",
					destPath.c_str(), readRes, writeRes);

				statistics.numErrors++;
				close(sourceFD);
				close(destFD);
				free(buf);

				EXIT_OR_RETURN_CONFIGURABLE(config.ignoreCopyErrors);
			}

			statistics.numBytesCopied += writeRes;
		}

		if(config.copyTimeUpdate)
		{ // update timestamps
			struct timespec newTimes[2] = {statBuf->st_atim, statBuf->st_mtim};

			int updateTimeRes = futimens(destFD, newTimes);
			if(updateTimeRes == -1)
			{
				fprintf(stderr, "Failed to update timestamps of copy destination file: %s; "
					"Error: %s\n", destPath.c_str(), strerror(errno) );

				statistics.numErrors++;
			}
		}

		// regular file copy complete => cleanup
		close(sourceFD);
		close(destFD);
		free(buf);
	}
	else
	{
		fprintf(stderr, "Skipping copy of entry due to non-regular file type. "
			"Path: %s\n", entryPath.c_str() );
		statistics.numFilesNotCopied++;
	}
}

/**
 * Unlink entry if it's not a directory.
 */
void unlinkEntry(const std::string& entryPath, const struct dirent* dirEntry,
	const struct stat* statBuf)
{
	if(!config.unlinkFiles)
		return;

	// config.statAll is forced to true when config.unlinkFiles is set

	if(S_ISDIR(statBuf->st_mode) )
		return;

	if(config.printVerbose)
		fprintf(stderr, "Unlinking: %s\n", entryPath.c_str() );

	int unlinkRes = unlink(entryPath.c_str() );
	if(unlinkRes == -1)
	{
		fprintf(stderr, "Failed to unlink file: %s; Error: %s\n",
			entryPath.c_str(), strerror(errno) );

		statistics.numErrors++;

		EXIT_OR_RETURN_CONFIGURABLE(config.ignoreUnlinkErrors);
	}
}

/**
 * Print entry either as plain newline-terminated string to console or in JSON format, depending
 * on config values.
 *
 * @dirEntry does not have to be provided if config.printJSON==false. otherwise it only needs to be
 * 		provided if statBuf is not provided, but there are special cases where it can still be
 * 		NULL, e.g. because it's a user-given path argument.
 * @statBuf does not have to be provided if config.printJSON==false. otherwise it only needs to be
 * 		provided if dirEntry->d_type==DT_UNKNOWN or config.statAll==true, but there are special
 * 		cases where it can still be NULL, e.g. if the stat() call returned an error.
 */
void printEntry(const std::string& entryPath, const struct dirent* dirEntry,
	const struct stat* statBuf)
{
	if(config.printEntriesDisabled)
		return;

	if(!config.printJSON)
	{ // simple print of path
		printf("%s%c", entryPath.c_str(), (config.print0 ? '\0' : '\n') );

		return;
	}

	// try to get dentry type from dirEntry or statBuf

	std::string dirEntryJSONType(DIRENTRY_JSON_TYPE_UNKNOWN);

	if(dirEntry && (dirEntry->d_type != DT_UNKNOWN) )
	{ // we can take type from dirEntry info
		switch(dirEntry->d_type)
		{
			case DT_BLK: 	dirEntryJSONType = DIRENTRY_JSON_TYPE_BLK; break;
			case DT_CHR: 	dirEntryJSONType = DIRENTRY_JSON_TYPE_CHR; break;
			case DT_DIR: 	dirEntryJSONType = DIRENTRY_JSON_TYPE_DIR; break;
			case DT_FIFO: 	dirEntryJSONType = DIRENTRY_JSON_TYPE_FIFO; break;
			case DT_LNK: 	dirEntryJSONType = DIRENTRY_JSON_TYPE_LNK; break;
			case DT_REG: 	dirEntryJSONType = DIRENTRY_JSON_TYPE_REG; break;
			case DT_SOCK: 	dirEntryJSONType = DIRENTRY_JSON_TYPE_SOCK; break;

			default:
			{ // should never happen (but we try to continue with "unknown" if it does)
				fprintf(stderr, "Encountered unexpected directory entry d_type. "
					"Path: %s; d_type: %u\n", entryPath.c_str(), (unsigned)dirEntry->d_type);
			} break;
		}
	}
	else
	if(statBuf)
	{ // take type from stat() info
		if(S_ISBLK(statBuf->st_mode) )
			dirEntryJSONType = DIRENTRY_JSON_TYPE_BLK;
		else
		if(S_ISCHR(statBuf->st_mode) )
			dirEntryJSONType = DIRENTRY_JSON_TYPE_CHR;
		else
		if(S_ISDIR(statBuf->st_mode) )
			dirEntryJSONType = DIRENTRY_JSON_TYPE_DIR;
		else
		if(S_ISFIFO(statBuf->st_mode) )
			dirEntryJSONType = DIRENTRY_JSON_TYPE_FIFO;
		else
		if(S_ISLNK(statBuf->st_mode) )
			dirEntryJSONType = DIRENTRY_JSON_TYPE_LNK;
		else
		if(S_ISREG(statBuf->st_mode) )
			dirEntryJSONType = DIRENTRY_JSON_TYPE_REG;
		else
		if(S_ISSOCK(statBuf->st_mode) )
			dirEntryJSONType = DIRENTRY_JSON_TYPE_SOCK;
		else
		{ // should never happen (but we try to continue with "unknown" if it does)
			fprintf(stderr, "Encountered unexpected directory entry stat st_mode. "
				"Path: %s; st_mode: %u\n", entryPath.c_str(), (unsigned)statBuf->st_mode);
		}
	}


	// print as JSON root object

	if(!config.statAll)
	{ // short JSON format
		printf("{"
			"\"path\":\"%s\","
			"\"type\":\"%s\""
			"}\n",
			escapeStrforJSON(entryPath).c_str(),
			dirEntryJSONType.c_str() );

		return;
	}

	// long JSON format
	// (note: statBuf might be NULL due to stat() error for this entry)

	if(statBuf)
	{
		printf("{"
			"\"path\":\"%s\","
			"\"type\":\"%s\","
			"\"st_dev\":\"%" PRIu64 "\","
			"\"st_ino\":\"%" PRIu64 "\","
			"\"st_mode\":\"%" PRIu64 "\","
			"\"st_nlink\":\"%" PRIu64 "\","
			"\"st_uid\":\"%" PRIu64 "\","
			"\"st_gid\":\"%" PRIu64 "\","
			"\"st_rdev\":\"%" PRIu64 "\","
			"\"st_size\":\"%" PRIu64 "\","
			"\"st_blksize\":\"%" PRIu64 "\","
			"\"st_blocks\":\"%" PRIu64 "\","
			"\"st_atime\":\"%" PRIu64 "\","
			"\"st_mtime\":\"%" PRIu64 "\","
			"\"st_ctime\":\"%" PRIu64 "\""
			"}\n",
			escapeStrforJSON(entryPath).c_str(),
			dirEntryJSONType.c_str(),
			(uint64_t)statBuf->st_dev,
			(uint64_t)statBuf->st_ino,
			(uint64_t)statBuf->st_mode,
			(uint64_t)statBuf->st_nlink,
			(uint64_t)statBuf->st_uid,
			(uint64_t)statBuf->st_gid,
			(uint64_t)statBuf->st_rdev,
			(uint64_t)statBuf->st_size,
			(uint64_t)statBuf->st_blksize,
			(uint64_t)statBuf->st_blocks,
			(uint64_t)statBuf->st_atime,
			(uint64_t)statBuf->st_mtime,
			(uint64_t)statBuf->st_ctime);
	}
	else
	{ // no statBuf (probably due to stat() error), so most fields are empty
		printf("{"
			"\"path\":\"%s\","
			"\"type\":\"%s\","
			"\"st_dev\":null,"
			"\"st_ino\":null,"
			"\"st_mode\":null,"
			"\"st_nlink\":null,"
			"\"st_uid\":null,"
			"\"st_gid\":null,"
			"\"st_rdev\":null,"
			"\"st_size\":null,"
			"\"st_blksize\":null,"
			"\"st_blocks\":null,"
			"\"st_atime\":null,"
			"\"st_mtime\":null,"
			"\"st_ctime\":null"
			"}\n",
			escapeStrforJSON(entryPath).c_str(),
			dirEntryJSONType.c_str() );
	}

}

/**
 * Filter discovered files/dirs and kick off processing of entries that came through the filters,
 * such as printing to console, copying etc.
 *
 * @dirEntry does not have to be provided if config.printJSON==false. otherwise it only needs to be
 * 		provided if statBuf is not provided, but there are special cases where it can still be
 * 		NULL, e.g. because it's a user-given path argument.
 * @statBuf does not have to be provided if config.printJSON==false. otherwise it only needs to be
 * 		provided if dirEntry->d_type==DT_UNKNOWN or config.statAll==true, but there are special
 * 		cases where it can still be NULL, e.g. if the stat() call returned an error.
 */
void processDiscoveredEntry(const std::string& entryPath, const struct dirent* dirEntry,
	const struct stat* statBuf)
{
	// filters

	if(!filterPrintEntryByType(entryPath, dirEntry, statBuf) )
		return;

	if(!filterPrintEntryByName(entryPath, dirEntry, statBuf) )
		return;

	if(!filterPrintEntryByPath(entryPath, dirEntry, statBuf) )
		return;

	if(!filterPrintEntryBySizeOrTime(entryPath, dirEntry, statBuf) )
		return;

	if(!filterPrintEntryByUIDAndGID(entryPath, dirEntry, statBuf) )
		return;

	// print entry

	printEntry(entryPath, dirEntry, statBuf);

	// exec system command on entry

	execSystemCommand(entryPath);

	// copy

	copyEntry(entryPath, dirEntry, statBuf);

	// unlink

	unlinkEntry(entryPath, dirEntry, statBuf);

	// note on quitAfterFirstMatch: we can't exit() here because of the other threads. and can't use
	// kill(0, SIGTERM) because that would exit with error code. so recursive scan() checks this.

	statistics.numFilterMatches++;
}

/**
 * This is the main workhorse. It does a breadth scan while dir stack size is below
 * config.depthSearchStartThreshold, in which cases discovered dirs are put on stack so that other
 * threads can grab them. Otherwise it switches to recursive depth search.
 */
void scan(std::string path, const unsigned short dirDepth)
{
	// stop in case of for quitAfterFirstMatch
	if(config.quitAfterFirstMatch && statistics.numFilterMatches)
		return;

	DIR* dirStream = opendir(path.c_str() );
	if(!dirStream)
	{
		statistics.numErrors++;

		int errnoBackup = errno;
		fprintf(stderr, "Failed to open dir: '%s'; Error: %s\n", path.c_str(), strerror(errno) );
		if( (errnoBackup == EACCES) || (errnoBackup == ENOENT) )
			return;

		kill(0, SIGTERM);
	}

	/* loop over contents of this entire directory - potentially recursively descending into subdirs
		along the way, depending on config and current global state */
	for ( ; ; )
	{
		errno = 0;
		struct dirent* dirEntry = readdir(dirStream);
		if(!dirEntry)
		{
			if(errno)
			{
				fprintf(stderr, "Failed to read from dir: %s; Error: %s\n",
					path.c_str(), strerror(errno) );

				statistics.numErrors++;
			}

			closedir(dirStream);
			return;
		}

		if(!strcmp(dirEntry->d_name, ".") || !strcmp(dirEntry->d_name, "..") )
			continue;

		struct stat statBuf;
		int statErrno = -1; // "-1" to let clear that statBuf is not usable yet

		// if dentry type is unknown then we have to stat to know if this is a dir to descend into
		if(config.statAll || (dirEntry->d_type == DT_UNKNOWN) )
		{
			statistics.numStatCalls++;

			int statRes = fstatat(dirfd(dirStream), dirEntry->d_name, &statBuf,
				AT_SYMLINK_NOFOLLOW);

			if(!statRes)
				statErrno = 0; // success, so mark statBuf as usable
			else
			{ // stat failed
				statErrno = errno;

				fprintf(stderr, "Failed to get attributes for path: %s; Error: %s\n",
					path.c_str(), strerror(statErrno) );
			}
		}

		if(dirEntry->d_type == DT_UNKNOWN)
			statistics.numUnknownFound++;

		std::string entryPath(path + "/" + dirEntry->d_name);

		if(dirEntry->d_type == DT_DIR ||
			( (dirEntry->d_type == DT_UNKNOWN) && !statErrno && S_ISDIR(statBuf.st_mode) ) )
		{ // this entry is a directory
			statistics.numDirsFound++;

			checkACLs(entryPath.c_str(), true);

			processDiscoveredEntry(entryPath, dirEntry, statErrno ? NULL : &statBuf);

			const bool doDescendDepth = (dirDepth < config.maxDirDepth);
			const bool doDescendMount = (config.filterMountID == (~0ULL) ) ? true :
				(!statErrno && (config.filterMountID == statBuf.st_dev) );

			if(doDescendMount && doDescendDepth)
			{
				if(sharedStack.getSize() >= config.depthSearchStartThreshold)
					scan(entryPath, dirDepth + 1);
				else // breadth search, so just add dir to stack for later processing
					sharedStack.push(entryPath, dirDepth + 1);
			}
		}
		else
		{ // this entry is not a directory (or unknown with stat() error)
			statistics.numFilesFound++;

			checkACLs(entryPath.c_str(), false);

			processDiscoveredEntry(entryPath, dirEntry, statErrno ? NULL : &statBuf);
		}

	}
}

/**
 * Starting point for directory structure scan threads.
 */
void threadStart()
{
	try
	{
		std::string dirPath;
		unsigned short dirDepth;

		while(sharedStack.popWait(dirPath, dirDepth) )
			scan(dirPath, dirDepth);
	}
	catch(ScanDoneException& e)
	{
	}
}

/**
 * Print summary at end of run.
 */
void printSummary()
{
	if(!config.printSummary)
		return; // summary disabled

	std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
	std::chrono::microseconds elapsedMicroSec =
		std::chrono::duration_cast<std::chrono::microseconds>(endTime - state.startTime);

	uint64_t elapsedSec = elapsedMicroSec.count() / 1000000;
	uint64_t elapsedMilliSecRemainder = (elapsedMicroSec.count() % 1000000) / 1000;

	uint64_t scanEntriesTotal = statistics.numDirsFound + statistics.numFilesFound;
	uint64_t scanEntriesPerSec =
		( (double)scanEntriesTotal / elapsedMicroSec.count() ) * 1000000;
	uint64_t copyMiBTotal = statistics.numBytesCopied / (1024*1024);
	uint64_t copyMiBPerSec = ( (double)copyMiBTotal / elapsedMicroSec.count() ) * 1000000;


	if(config.printVerbose)
	{
		std::cerr << "CONFIG:" << std::endl;
		std::cerr << "  * threads:       " << config.numThreads << std::endl;
		std::cerr << "  * godeep:        " << config.depthSearchStartThreshold << std::endl;
		std::cerr << "  * flags:         " <<
			"stat: " << config.statAll << "; " <<
			"aclcheck: " << config.checkACLs << std::endl;
	}


	try
	{
		std::cerr.imbue(std::locale("") );
	}
	catch(std::runtime_error& exception)
	{
		/* static binaries from Alpine fail on Ubuntu with std::runtime_error
			"locale::facet::_S_create_c_locale name not valid". however, this is just for nicer
			number formatting, so not ciritical.*/
	}

	std::cerr << "STATISTICS:" << std::endl;

	std::cerr << "  * entries found: " <<
			"files: " << statistics.numFilesFound << "; " <<
			"dirs: " << statistics.numDirsFound << "; " <<
			"filter matches: " << statistics.numFilterMatches << std::endl;

	std::cerr << "  * special cases: " <<
			"unknown type: " << statistics.numUnknownFound << "; " <<
			"errors: " << statistics.numErrors << std::endl;

	if(statistics.numStatCalls)
		std::cerr << "  * stat calls:    " << statistics.numStatCalls << std::endl;

	if(config.checkACLs)
		std::cerr << "  * ACLs found:    " <<
			statistics.numAccessACLsFound << " access; " <<
			statistics.numDefaultACLsFound << " default" << std::endl;

	std::cerr << "  * scan speed:    " <<
		scanEntriesPerSec << " entries/s; " <<
		"runtime: " << elapsedSec << "." <<	std::setfill('0') << std::setw(3) <<
			elapsedMilliSecRemainder << "s" << std::endl;

	if(!config.copyDestDir.empty() )
		std::cerr << "  * copy speed:    " <<
			copyMiBPerSec << " MiB/s; " <<
			"total: " << copyMiBTotal << " MiB; " <<
			"skipped files: " << statistics.numFilesNotCopied << std::endl;
}

void printUsageAndExit()
{
	std::cout << EXE_NAME " - Parallel search for files & dirs" << std::endl;
	std::cout << std::endl;
	std::cout << "VERSION: " EXE_VERSION << std::endl;
	std::cout << std::endl;
	std::cout << "USAGE: " EXE_NAME " [OPTIONS...] [PATHS...]" << std::endl;
	std::cout << std::endl;
	std::cout << "OPTIONS (in alphabetical order):" << std::endl;
	std::cout << "  --atime NUM       - atime filter based on number of days in the past." << std::endl;
	std::cout << "                      +/- prefix to match older or more recent values." << std::endl;
	std::cout << "  --aclcheck        - Query ACLs of all discovered entries." << std::endl;
	std::cout << "                      (Just for testing, does not change the result set.)" << std::endl;
	std::cout << "  --copyto PATH     - Copy discovered files and dirs to this directory." << std::endl;
	std::cout << "                      Only regular files, dirs and symlinks will be copied." << std::endl;
	std::cout << "                      Hardlinks will not be preserved. Source and" << std::endl;
	std::cout << "                      destination have to be dirs." << std::endl;
	std::cout << "  --ctime NUM       - ctime filter based on number of days in the past." << std::endl;
	std::cout << "                      +/- prefix to match older or more recent values." << std::endl;
	std::cout << "  --exec CMD ARGs ; - Execute the given system command and arguments for each" << std::endl;
	std::cout << "                      discovered file/dir. The string '{}' in any arg will get" << std::endl;
	std::cout << "                      replaced by the current file/dir path. The argument ';'" << std::endl;
	std::cout << "                      marks the end of the command line to run." << std::endl;
	std::cout << "                      (Example: elfindo --exec ls -lhd '{}' \\; --type d)" << std::endl;
	std::cout << "  --gid NUM         - Filter based on numeric group ID." << std::endl;
	std::cout << "  --godeep NUM      - Threshold to switch from breadth to depth search." << std::endl;
	std::cout << "                      (Default: number of scan threads)" << std::endl;
	std::cout << "  --group STR       - Filter based on group name or numeric group ID." << std::endl;
	std::cout << "  --json            - Print entries in JSON format. Each file/dir is a" << std::endl;
	std::cout << "                      separate JSON root object. Contained data depends on" << std::endl;
	std::cout << "                      whether \"--" ARG_STAT_LONG "\" is given." << std::endl;
	std::cout << "                      (Hint: Consider the \"jq\" tool to filter results.)" << std::endl;
	std::cout << "  --maxdepth        - Max directory depth to scan. (Path arguments have" << std::endl;
	std::cout << "                      depth 0.)" << std::endl;
	std::cout << "  --mount           - Alias for \"--xdev\"." << std::endl;
	std::cout << "  --mtime NUM       - mtime filter based on number of days in the past." << std::endl;
	std::cout << "                      +/- prefix to match older or more recent values." << std::endl;
	std::cout << "  --name PATTERN    - Filter on name of file or current dir. Pattern may" << std::endl;
	std::cout << "                      contain '*' & '?' as wildcards. This parameter can be" << std::endl;
	std::cout << "                      given multiple times, in which case filenames matching" << std::endl;
	std::cout << "                      any of the given patterns will pass." << std::endl;
	std::cout << "                      the filter." << std::endl;
	std::cout << "  --newer PATH      - Filter based on more recent mtime than given path." << std::endl;
	std::cout << "  --noprint         - Do not print names of discovered files and dirs." << std::endl;
	std::cout << "  --nosummary       - Disable summary output to stderr." << std::endl;
	std::cout << "  --notimeupd       - Do not update atime/mtime of copied files." << std::endl;
	std::cout << "  --path PATTERN    - Filter on path of discovered entries." << std::endl;
	std::cout << "                      Pattern may contain '*' & '?' as wildcards." << std::endl;
	std::cout << "  --print0          - Terminate printed entries with null instead of newline." << std::endl;
	std::cout << "                      (Hint: This goes nicely with \"xargs -0\".)" << std::endl;
	std::cout << "  --quit            - Terminate after first match. (Note: With multiple threads" << std::endl;
	std::cout << "                      it's possible that more than one match gets printed." << std::endl;
	std::cout << "                      Consider combining this with \"| head -n 1\".)" << std::endl;
	std::cout << "  --size NUM        - Size filter." << std::endl;
	std::cout << "                      +/- prefix to match greater or smaller values." << std::endl;
	std::cout << "                      Default unit is 512-byte blocks." << std::endl;
	std::cout << "                      'c' suffix to specify bytes instead of 512-byte blocks." << std::endl;
	std::cout << "                      'k'/'M'/'G' suffix for KiB/MiB/GiB units." << std::endl;
	std::cout << "  --stat            - Query attributes of all discovered files & dirs." << std::endl;
	std::cout << "  -t, --threads NUM - Number of scan threads. (Default: 16)" << std::endl;
	std::cout << "  --type TYPE       - Search type. 'f' for regular files, 'd' for directories." << std::endl;
	std::cout << "  --uid NUM         - Filter based on numeric user ID." << std::endl;
	std::cout << "  --unlink          - Delete discovered files, not dirs." << std::endl;
	std::cout << "  --user STR        - Filter based on user name or numeric user ID." << std::endl;
	std::cout << "  --verbose         - Enable verbose output." << std::endl;
	std::cout << "  --version         - Print version and exit." << std::endl;
	std::cout << "  --xdev            - Don't descend directories on other filesystems." << std::endl;
	std::cout << std::endl;
	std::cout << "Examples:" << std::endl;
	std::cout << "  Find all files and dirs under /data/mydir:" << std::endl;
	std::cout << "    $ " EXE_NAME " /data/mydir" << std::endl;
	std::cout << std::endl;
	std::cout << "  Find all regular files that haven't been accessed within the last 3 days:" << std::endl;
	std::cout << "    $ " EXE_NAME " --atime +3 /data/mydir" << std::endl;
	std::cout << std::endl;
	std::cout << "  Find all regular files and send 0-terminated paths to xargs for" << std::endl;
	std::cout << "  parallel \"ls -lh\":" << std::endl;
	std::cout << "    $ " EXE_NAME " --type f --print0 /data/mydir | \\" << std::endl;
	std::cout << "      xargs -P 16 -r -0 -n 10 \\" << std::endl;
	std::cout << "      ls -lh" << std::endl;
	std::cout << std::endl;
	std::cout << "  Filter JSON output using jq and send 0-terminated paths to xargs for" << std::endl;
	std::cout << "  parallel \"ls -lh\":" << std::endl;
	std::cout << "    $ " EXE_NAME " --json /data/mydir | \\" << std::endl;
	std::cout << "      jq -rj '.|select(.type==\"regfile\")|(.path + \"\\u0000\")' | \\" << std::endl;
	std::cout << "      xargs -P 16 -r -0 -n 10 \\" << std::endl;
	std::cout << "      ls -lh" << std::endl;

	exit(EXIT_FAILURE);
}

void printVersionAndExit()
{
	std::cout << EXE_NAME << std::endl;
	std::cout << " * Version: " EXE_VERSION << std::endl;
	std::cout << " * Build date: " __DATE__ << " " << __TIME__ << std::endl;

	exit(EXIT_SUCCESS);
}

/**
 * Parse the suffix of the "--size" argument (if any) and return the bytes value without suffix.
 */
std::string parseSizeArgSuffix(std::string userVal)
{
	if(userVal.empty() )
		return userVal;

	char userValSuffix = userVal[userVal.length() - 1];

	// no suffix => default is 512 byte blocks
	if( (userValSuffix >= '0') && (userValSuffix <= '9') )
		return std::to_string(std::stoull(userVal) * 512);

	userVal.pop_back(); // remove suffix, we have userValSuffix as copy

	// see "find" man page for meaning of suffixes
	switch(userValSuffix)
	{
		case 'b': return std::to_string(std::stoull(userVal) * 512);
		case 'c': return userVal;
		case 'w': return std::to_string(std::stoull(userVal) * 2);
		case 'k': return std::to_string(std::stoull(userVal) * 1024);
		case 'M': return std::to_string(std::stoull(userVal) * 1024 * 1024);
		case 'G': return std::to_string(std::stoull(userVal) * 1024 * 1024 * 1024);

		default:
		{
			fprintf(stderr, "Invalid suffix: %s\n", userVal.c_str() );

			exit(EXIT_FAILURE);
		}
	}
}

/**
 * Simplified way to call parseExactLessGreaterVal().
 * Example: PARSE_EXACT_LESS_GREATER_VAL(optarg, atime, ATIME)
 */
#define _PARSE_EXACT_LESS_GREATER_VAL(userVal, lowercaseCfgName, uppercaseCfgName) \
	parseExactLessGreaterVal(userVal, \
		config.filterSizeAndTime.lowercaseCfgName ## Exact, \
		config.filterSizeAndTime.lowercaseCfgName ## Less, \
		config.filterSizeAndTime.lowercaseCfgName ## Greater, \
		FILTER_FLAG_ ## uppercaseCfgName ## _EXACT, \
		FILTER_FLAG_ ## uppercaseCfgName ## _LESS, \
		FILTER_FLAG_ ## uppercaseCfgName ## _GREATER)
#define PARSE_EXACT_LESS_GREATER_VAL(userVal, lowercaseCfgName, uppercaseCfgName) \
	_PARSE_EXACT_LESS_GREATER_VAL(userVal, lowercaseCfgName, uppercaseCfgName)

/**
 * Parse user-given value args (e.g. for size or mtime) that can be set to exact (no prefix),
 * less ("-" prefix) or greater ("+" prefix) matches.
 * {exaclt,less,greater}CfgFlag are the corresponding values for filterSizeAndTimeFlags.
 */
void parseExactLessGreaterVal(std::string userVal,
	uint64_t& exactCfgVal, uint64_t& lessCfgVal, uint64_t& greaterCfgVal,
	unsigned exactCfgFlag, unsigned lessCfgFlag, unsigned greaterCfgFlag)
{
	if(userVal.empty() )
		return;

	config.statAll = true; // need stat() info for time/size filtering

	// handle +/- prefix, set actualCfgValPtr and corresponding filter flag

	uint64_t* actualCfgValPtr;

	if(userVal[0] == '-')
	{ // match less than userVal for size and "less in the past" (so greater) for timestamps
		userVal.erase(0, 1); // remove '-' char

		if(exactCfgFlag == FILTER_FLAG_SIZE_EXACT)
		{ // size => '-' means "less"
			actualCfgValPtr = &lessCfgVal;
			config.filterSizeAndTime.filterSizeAndTimeFlags |= lessCfgFlag;
		}
		else
		{ // time => '-' means "greater" (less in the past)
			actualCfgValPtr = &greaterCfgVal;
			config.filterSizeAndTime.filterSizeAndTimeFlags |= greaterCfgFlag;
		}
	}
	else
	if(userVal[0] == '+')
	{ // match greater than userVal for size and "more in the past" (so less) for timestamps
		userVal.erase(0, 1); // remove '+' char

		if(exactCfgFlag == FILTER_FLAG_SIZE_EXACT)
		{ // size => '+' means "greater"
			actualCfgValPtr = &greaterCfgVal;
			config.filterSizeAndTime.filterSizeAndTimeFlags |= greaterCfgFlag;
		}
		else
		{ // time => '+' means "less" (further in the past)
			actualCfgValPtr = &lessCfgVal;
			config.filterSizeAndTime.filterSizeAndTimeFlags |= lessCfgFlag;
		}
	}
	else
	{ // match exact
		actualCfgValPtr = &exactCfgVal;
		config.filterSizeAndTime.filterSizeAndTimeFlags |= exactCfgFlag;
	}

	// actualCfgValPtr is set, but we didn't assign the value yet at this point

	if(exactCfgFlag == FILTER_FLAG_SIZE_EXACT)
	{ // size can have a suffix
		userVal = parseSizeArgSuffix(userVal);
		*actualCfgValPtr = std::stoull(userVal);
	}
	else
	{ // {a,c,m}time, so we have to substract user val times 24h from current time
		const uint64_t secsPerDay = 60 * 60 * 24;

		time_t nowT = time(NULL);

		*actualCfgValPtr = nowT - (std::stoull(userVal) * secsPerDay);
	}
}

/**
 * Get mtime of given file and set newer mtime filter.
 */
void setFileNewerFilterConfig(const char* path)
{
	config.statAll = true; // need stat() info for time filtering

	struct stat statBuf;

	int statRes = stat(path, &statBuf);

	if(statRes)
	{
		fprintf(stderr, "Failed to get attributes of path: %s; Error: %s\n",
			path, strerror(errno) );

		exit(EXIT_FAILURE);
	}

	if(config.printVerbose)
		fprintf(stderr, "Setting newer mtime filter based on given path: %s; "
			"Seconds since epoch: %" PRIu64 "\n",
			path, (uint64_t)statBuf.st_mtim.tv_sec);

	config.filterSizeAndTime.filterSizeAndTimeFlags |= FILTER_FLAG_MTIME_GREATER;
	config.filterSizeAndTime.mtimeGreater = (uint64_t)statBuf.st_mtim.tv_sec;
}

/**
 * Remove an argument from the command line argv and decrease argc. Remove means that everything
 * after this element will be shifted to the front.
 *
 * @param deleteIdx zero-based index of element to be deleted.
 */
void removeCmdLineArg(int deleteIdx, int& argc, char** argv)
{
	for(int i=(deleteIdx+1); i < argc; i++)
	{
		argv[i-1] = argv[i];
	}

	argc--;

	argv[argc] = NULL; // as per definition in C standard
}

/**
 * Parse arguments to find ARG_EXEC_LONG and copy all following args until excluding
 * ARG_EXEC_TERMINATOR to config. ARG_EXEC_LONG and following args until including
 * ARG_EXEC_TERMINATOR will be removed from argv & argc.
 */
void parseExecArguments(int& argc, char** argv)
{
	for(int argIdx=1; argIdx < argc; argIdx++)
	{
		if( (argv[argIdx] == std::string("-" ARG_EXEC_LONG) ) ||
			(argv[argIdx] == std::string("--" ARG_EXEC_LONG) ) )
		{ // we found exec start, now take all following args until EXEC_ARG_TERMINATOR
			int execArgsIdx = argIdx;

			// remove ARG_EXEC_LONG
			removeCmdLineArg(argIdx, argc, argv);

			// (note: no idx advance in loop because removeCmdLineArg removes current elem)

			while(execArgsIdx < argc)
			{
				// (note: "!empty" below because first arg has to be name of executable)
				if(!config.exec.cmdLineStrVec.empty() &&
					(argv[execArgsIdx] == std::string(EXEC_ARG_TERMINATOR) ) )
				{ // we found the args terminator
					removeCmdLineArg(execArgsIdx, argc, argv);
					return; // terminator found, so we're done
				}

				// executable or normal arg => just move over to exec config
				config.exec.cmdLineStrVec.push_back(argv[execArgsIdx] );
				removeCmdLineArg(execArgsIdx, argc, argv);
			}

			if(execArgsIdx == argc)
			{
				fprintf(stderr, "Missing terminator ';' in 'exec' arguments list\n");
				exit(EXIT_FAILURE);
			}

			// we found and processed the 'exec' parameters, so we're done here
			return;
		}
	}
}

/**
 * Parse commmand line arguments and set corresponding config values.
 */
void parseArguments(int argc, char** argv)
{
	bool needFilterByDevIDInit = false; // true for delayed filter by mount ID init

	/* note: this removes the "exec" arg and all following up to the terminator from argv,
	 	 because getopt_long_only() below can change order of arguments in argv */
	parseExecArguments(argc, argv);


	for( ; ; )
	{
		/* struct option:
			* (1) name: is the name of the long option.
			* (2) has_arg:
				* no_argument (or 0) if the option does not take an argument;
				* required_argument (or 1) if the option requires an argument;
				* optional_argument (or 2) if the option takes an optional argument.
			* (3) flag: specifies how results are returned for a long option. If flag is NULL, then
				getopt_long() returns val. (For example, the calling program may set val to the
				equivalent short option character.) Otherwise, getopt_long() returns 0, and flag
				points to a variable which is set to val if the option is found, but left unchanged
				if the option is not found.
			* (4) val: the value to return, or to load into the variable pointed to by flag.
		*/
		static struct option long_options[] =
		{
				{ ARG_ACLCHECK_LONG, no_argument, 0, 0 },
				{ ARG_COPYDEST_LONG, required_argument, 0, 0 },
				{ ARG_EXEC_LONG, no_argument, 0, 0 },
				{ ARG_FILTER_ATIME, required_argument, 0, 0 },
				{ ARG_FILTER_CTIME, required_argument, 0, 0 },
				{ ARG_FILTER_MTIME, required_argument, 0, 0 },
				{ ARG_FILTER_SIZE, required_argument, 0, 0 },
				{ ARG_GID_LONG, required_argument, 0, 0 },
				{ ARG_GODEEP_LONG, required_argument, 0, 0 },
				{ ARG_GROUP_LONG, required_argument, 0, 0 },
				{ ARG_HELP_LONG, no_argument, 0, ARG_HELP_SHORT },
				{ ARG_JSON_LONG, no_argument, 0, 0 },
				{ ARG_MAXDEPTH_LONG, required_argument, 0, 0 },
				{ ARG_MOUNT_LONG, no_argument, 0, 0 },
				{ ARG_NAME_LONG, required_argument, 0, 0 },
				{ ARG_NEWER_LONG, required_argument, 0, 0 },
				{ ARG_NOCOPYERR_LONG, no_argument, 0, 0 },
				{ ARG_NODELERR_LONG, no_argument, 0, 0 },
				{ ARG_NOPRINT_LONG, no_argument, 0, 0 },
				{ ARG_NOSUMMARY_LONG, no_argument, 0, 0 },
				{ ARG_NOTIMEUPD_LONG, no_argument, 0, 0 },
				{ ARG_PATH_LONG, required_argument, 0, 0 },
				{ ARG_PRINT0_LONG, no_argument, 0, 0 },
				{ ARG_QUITAFTER1_LONG, no_argument, 0, 0 },
				{ ARG_SEARCHTYPE_LONG, required_argument, 0, 0 },
				{ ARG_STAT_LONG, no_argument, 0, 0 },
				{ ARG_THREADS_LONG, required_argument, 0, ARG_THREADS_SHORT },
				{ ARG_UID_LONG, required_argument, 0, 0 },
				{ ARG_UNLINK_LONG, no_argument, 0, 0 },
				{ ARG_USER_LONG, required_argument, 0, 0 },
				{ ARG_VERBOSE_LONG, no_argument, 0, 0 },
				{ ARG_VERSION_LONG, no_argument, 0, 0 },
				{ ARG_XDEV_LONG, no_argument, 0, 0 },
				{ 0, 0, 0, 0 } // all-zero is the terminating element
		};

		int longOptionIndex = 0;

		// warning: getopt_long_only can re-order arguments in argv
		/* currentOption:
		   * for short opts: the option character
		   * for long opts: they return val if flag (3rd val) is NULL, and 0 otherwise.
		   * -1: if all options parsed
		   * '?': undefined/extraneous option
		   * ':': for missing arguments if first char in optstring is ':', otherwise '?'
		*/
		int currentOption = getopt_long_only(argc, argv, "t:d:h", long_options, &longOptionIndex);

		if(currentOption == -1)
			break; // done; all options parsed

		switch(currentOption)
		{
			case 0: // long option
			{
				std::string currentOptionName = long_options[longOptionIndex].name;

				if(ARG_ACLCHECK_LONG == currentOptionName)
					config.checkACLs = true;
				else
				if(ARG_COPYDEST_LONG == currentOptionName)
				{
					config.copyDestDir = optarg;
					config.statAll = true; // to be able to rely on type in statBuf and for mtime
				}
				else
				if(ARG_EXEC_LONG == currentOptionName)
				{
					// error out if exec is still found here, because it means it existed twice
					// (actual exec arg handling intentionally happens before getopt_long_only() )
					fprintf(stderr, "Aborting because '" ARG_EXEC_LONG "' option is given more "
						"than once.\n");
					exit(EXIT_FAILURE);
				}
				else
				if(ARG_FILTER_ATIME == currentOptionName)
					PARSE_EXACT_LESS_GREATER_VAL(optarg, atime, ATIME);
				else
				if(ARG_FILTER_CTIME == currentOptionName)
					PARSE_EXACT_LESS_GREATER_VAL(optarg, ctime, CTIME);
				else
				if(ARG_FILTER_MTIME == currentOptionName)
					PARSE_EXACT_LESS_GREATER_VAL(optarg, mtime, MTIME);
				else
				if(ARG_FILTER_SIZE == currentOptionName)
					PARSE_EXACT_LESS_GREATER_VAL(optarg, size, SIZE);
				else
				if(ARG_GID_LONG == currentOptionName)
				{
					config.filterGID = std::stoull(optarg);

					config.statAll = true; // we need statBuf for this filter
				}
				else
				if(ARG_GODEEP_LONG == currentOptionName)
					config.depthSearchStartThreshold = std::atoi(optarg);
				else
				if(ARG_GROUP_LONG == currentOptionName)
				{
					if(strlen(optarg) && isdigit(optarg[0]) )
						config.filterGID = std::stoull(optarg);
					else
					{
						struct group* groupEntry = getgrnam(optarg);
						if(!groupEntry)
						{
							fprintf(stderr, "Aborting because given group name could not be"
								"resolved to numeric GID. Does the group exist? Group: %s\n",
								optarg);
							exit(EXIT_FAILURE);
						}

						config.filterGID = groupEntry->gr_gid;
					}

					config.statAll = true; // we need statBuf for this filter
				}
				else
				if(ARG_JSON_LONG == currentOptionName)
					config.printJSON = true;
				else
				if(ARG_MAXDEPTH_LONG == currentOptionName)
					config.maxDirDepth = std::atoi(optarg);
				else
				if( (ARG_MOUNT_LONG == currentOptionName) ||
					(ARG_XDEV_LONG == currentOptionName) )
				{
					/* we can't init dev ID here yet because we have not initialized the mount
						mount points yet. */
					needFilterByDevIDInit = true;

					config.statAll = true; // we need statBuf for this filter
				}
				else
				if(ARG_NAME_LONG == currentOptionName)
					config.nameFilterVec.push_back(optarg);
				else
				if(ARG_NEWER_LONG == currentOptionName)
					setFileNewerFilterConfig(optarg);
				else
				if(ARG_NOCOPYERR_LONG == currentOptionName)
					config.ignoreCopyErrors = true;
				else
				if(ARG_NODELERR_LONG == currentOptionName)
					config.ignoreUnlinkErrors = true;
				else
				if(ARG_NOPRINT_LONG == currentOptionName)
					config.printEntriesDisabled = true;
				else
				if(ARG_NOSUMMARY_LONG == currentOptionName)
					config.printSummary = false;
				else
				if(ARG_NOTIMEUPD_LONG == currentOptionName)
					config.copyTimeUpdate = false;
				else
				if(ARG_PATH_LONG == currentOptionName)
					config.pathFilter = optarg;
				else
				if(ARG_PRINT0_LONG == currentOptionName)
					config.print0 = true;
				else
				if(ARG_QUITAFTER1_LONG == currentOptionName)
					config.quitAfterFirstMatch = true;
				else
				if(ARG_SEARCHTYPE_LONG == currentOptionName)
					config.searchType = (strlen(optarg) ? optarg[0] : 0);
				else
				if(ARG_STAT_LONG == currentOptionName)
					config.statAll = true;
				else
				if(ARG_UID_LONG == currentOptionName)
				{
					config.filterUID = std::stoull(optarg);

					config.statAll = true; // we need statBuf for this filter
				}
				else
				if(ARG_UNLINK_LONG == currentOptionName)
				{
					config.unlinkFiles = true;
					config.statAll = true; // to be able to rely on type in statBuf for dir vs file
				}
				else
				if(ARG_USER_LONG == currentOptionName)
				{
					if(strlen(optarg) && isdigit(optarg[0]) )
						config.filterUID = std::stoull(optarg);
					else
					{
						struct passwd* passwdEntry = getpwnam(optarg);
						if(!passwdEntry)
						{
							fprintf(stderr, "Aborting because given user name could not be "
								"resolved to numeric UID. Does the user exist? User: %s\n", optarg);
							exit(EXIT_FAILURE);
						}

						config.filterUID = passwdEntry->pw_uid;
					}

					config.statAll = true; // we need statBuf for this filter
				}
				else
				if(ARG_VERBOSE_LONG == currentOptionName)
					config.printVerbose = true;
				else
				if(ARG_VERSION_LONG == currentOptionName)
					config.printVersion = true;
			} break;

			case ARG_HELP_SHORT:
				printUsageAndExit();
			break;

			case ARG_THREADS_SHORT:
				config.numThreads = std::atoi(optarg);
			break;

			case '?': // unknown (long or short) option
				fprintf(stderr, "Aborting due to unrecognized option\n");
				exit(EXIT_FAILURE);
			break;

			default:
				fprintf(stderr, "?? getopt returned character code 0%o ??\n", currentOption);
		}
	}

	// parse non-option arguments
	if(optind < argc)
	{
		if(config.printVerbose)
			fprintf(stderr, "Non-option arguments: ");

		while(optind < argc)
		{
			if(config.printVerbose)
				fprintf(stderr, "%s ", argv[optind]);

			config.scanPaths.push_back(argv[optind] );
			optind++;
		}

		if(config.printVerbose)
			fprintf(stderr, "\n");
	}

	// init config defaults
	if(!config.depthSearchStartThreshold)
		config.depthSearchStartThreshold = config.numThreads;


	// sanity check

	if(!config.copyDestDir.empty() && (config.scanPaths.size() > 1) )
	{
		fprintf(stderr, "Only a single scan path may be given when "
			"\"--" ARG_COPYDEST_LONG "\" is used\n");
		exit(EXIT_FAILURE);
	}

	/* delayed dev ID init to not descend into other mountpoints...
		(init of this is here because we need to have scan paths initialized.) */
	if(needFilterByDevIDInit)
	{
		struct stat statBuf;

		int statRes = stat(config.scanPaths.empty() ? "." : config.scanPaths.front().c_str(),
			&statBuf);

		if(statRes != 0)
		{
			fprintf(stderr, "Aborting because dev ID retrieval for scan path failed.\n");
			exit(EXIT_FAILURE);
		}

		config.filterMountID = statBuf.st_dev;
	}

}

int main(int argc, char** argv)
{
	int retVal = EXIT_SUCCESS;

	parseArguments(argc, argv);

	if(config.printVersion)
		printVersionAndExit();

	if(config.scanPaths.empty() )
		config.scanPaths.push_back("."); // if no paths given then scan current dir

	const unsigned short currentDirDepth = 0;

	// check entry type of user-given paths and add dirs to stack
	for(std::string currentPath : config.scanPaths)
	{
		struct stat statBuf;

		int statRes = lstat(currentPath.c_str(), &statBuf);

		if(statRes)
		{
			int errnoBackup = errno;

			fprintf(stderr, "Failed to get attributes for path: %s; Error: %s\n",
				currentPath.c_str(), strerror(errno) );

			retVal = EXIT_FAILURE;

			if( (errnoBackup == EACCES) || (errnoBackup == ENOENT) )
				continue;

			// terminate if error is not just EACCESS or ENOENT
			kill(0, SIGTERM);
		}

		if(S_ISDIR(statBuf.st_mode) )
		{ // this entry is a directory
			processDiscoveredEntry(currentPath, NULL, &statBuf);

			if(currentDirDepth < config.maxDirDepth)
			{
				/* mimic gnu findutils behavior to preserve the given number of trailing slashes.
					our scan() func will always add one slash, so we have to remove one here if
					any */
				std::string currentPathTrimmed(currentPath);
				if( (currentPathTrimmed != "/") &&
					(currentPathTrimmed[currentPathTrimmed.length()-1] == '/') )
					currentPathTrimmed.erase(currentPathTrimmed.length()-1, 1);

				sharedStack.push(currentPathTrimmed, currentDirDepth + 1);
			}
		}
		else
		{ // this entry is not a directory
			processDiscoveredEntry(currentPath, NULL, &statBuf);
		}
	}

	// with single thread, always do depth search because there is no parallelism anyways
	if(config.numThreads == 1)
		config.depthSearchStartThreshold = 0;

	// start threads
	for(unsigned i=0; i < config.numThreads; i++)
		state.scanThreads.push(std::thread(threadStart) );

	// wait for threads to self-terminate
	while(!state.scanThreads.empty() )
	{
		state.scanThreads.top().join();
		state.scanThreads.pop();
	}

	printSummary();

	return retVal;
}

