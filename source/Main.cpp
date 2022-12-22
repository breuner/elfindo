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
#include <inttypes.h> // defines PRIu64 for printf uint64_t
#include <iostream>
#include <iomanip>
#include <libgen.h>
#include <list>
#include <mutex>
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


#define ARG_FILTER_ATIME	"atime"
#define ARG_ACLCHECK_LONG	"aclcheck"
#define ARG_FILTER_CTIME	"ctime"
#define ARG_GODEEP_LONG		"godeep"
#define ARG_HELP_LONG		"help"
#define ARG_HELP_SHORT		'h'
#define ARG_JSON_LONG		"json"
#define ARG_MAXDEPTH_LONG	"maxdepth"
#define ARG_FILTER_MTIME	"mtime"
#define ARG_NAME_LONG		"name"
#define ARG_NOSUMMARY_LONG	"nosummary"
#define ARG_PATH_LONG		"path"
#define ARG_PRINT0_LONG		"print0"
#define ARG_FILTER_SIZE		"size"
#define ARG_STAT_LONG		"stat"
#define ARG_THREADS_LONG	"threads"
#define ARG_THREADS_SHORT	't'
#define ARG_SEARCHTYPE_LONG	"type"
#define ARG_VERBOSE_LONG	"verbose"
#define ARG_VERSION_LONG	"version"
#define ARG_COPYDEST_LONG	"copyto"

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
	std::string nameFilter; // filter on filename (in contrast to full path)
	std::string pathFilter; // filter on full path
	struct
	{
		uint64_t sizeExact {0}, sizeLess {0}, sizeGreater {0};
		uint64_t mtimeExact {0}, mtimeLess {0}, mtimeGreater {0};
		uint64_t ctimeExact {0}, ctimeLess {0}, ctimeGreater {0};
		uint64_t atimeExact {0}, atimeLess {0}, atimeGreater {0};

		unsigned filterSizeAndTimeFlags; // FILTER_FLAG_..._{EXACT,LESS,GREATER} flags
	} filterSizeAndTime;
	std::string copyDestDir; // target dir for file/dir copies
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
	if(config.nameFilter.empty() )
		return true; // no filter defined by user => always passes

	bool isFile =
		(dirEntry && (dirEntry->d_type != DT_UNKNOWN) && (dirEntry->d_type != DT_DIR) ) ||
		(statBuf && !S_ISDIR(statBuf->st_mode) );

	if(!isFile)
		return false; // anything that's not a file can't match

	int matchRes = fnmatch(config.nameFilter.c_str(),
		std::filesystem::path(entryPath).filename().string().c_str(), 0 /* flags */);

	if(!matchRes)
		return true; // we have a match

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
		int mkRes = mkdir(destPath.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
		if(mkRes && (errno != EEXIST) )
		{
			fprintf(stderr, "Failed to create dir: %s; Error: %s\n",
				destPath.c_str(), strerror(errno) );

			statistics.numErrors++;
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
			exit(1);
		}

		ssize_t readRes = readlink(entryPath.c_str(), buf, bufSize);
		if(readRes == bufSize)
		{
			fprintf(stderr, "Failed to copy symlink due to long target path: %s; Max: %u\n",
				entryPath.c_str(), bufSize);

			statistics.numErrors++;
			free(buf);
			return;
		}
		else
		if(readRes == -1)
		{
			fprintf(stderr, "Failed to read symlink for copying: %s; Error: %s\n",
				entryPath.c_str(), strerror(errno) );

			statistics.numErrors++;
			free(buf);
			return;
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
			return;
		}

		// symlink copy done => cleanup
		free(buf);
	}
	else
	if(S_ISREG(statBuf->st_mode) )
	{ // copy regular file
		int sourceFD = open(entryPath.c_str(), O_RDONLY);
		if(sourceFD == -1)
		{
			fprintf(stderr, "Failed to open copy source file for reading: %s; Error: %s\n",
				entryPath.c_str(), strerror(errno) );

			statistics.numErrors++;
			return;
		}

		int destFD = open(destPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY,
			S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
		if(destFD == -1)
		{
			fprintf(stderr, "Failed to open copy destination file for writing: %s; Error: %s\n",
				destPath.c_str(), strerror(errno) );

			statistics.numErrors++;
			close(sourceFD);
			return;
		}

		const unsigned bufSize = 4*1024*1024; // 4MiB copy buffer
		char* buf = (char*)malloc(bufSize);
		ssize_t readRes;

		if(!buf)
		{
			fprintf(stderr, "Failed to allocate memory buffer for file copy. Alloc size: %u\n",
				bufSize);
			exit(1);
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
				return;
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
				return;
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
				return;
			}

			statistics.numBytesCopied += writeRes;
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
 * Print entry either as plain newline-terminated string to console or in JSON format, depending
 * on config values.
 *
 * This also contains the filtering and file/dir copying calls.
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
	// filters

	if(!filterPrintEntryByType(entryPath, dirEntry, statBuf) )
		return;

	if(!filterPrintEntryByName(entryPath, dirEntry, statBuf) )
		return;

	if(!filterPrintEntryByPath(entryPath, dirEntry, statBuf) )
		return;

	if(!filterPrintEntryBySizeOrTime(entryPath, dirEntry, statBuf) )
		return;

	// copy

	copyEntry(entryPath, dirEntry, statBuf);

	// print entry

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
 * This is the main workhorse. It does a breadth scan while dir stack size is below
 * config.depthSearchStartThreshold, in which cases discovered dirs are put on stack so that other
 * threads can grab them. Otherwise it switches to recursive depth search.
 */
void scan(std::string path, const unsigned short dirDepth)
{
	DIR* dirStream = opendir(path.c_str() );
	if(!dirStream)
	{
		statistics.numErrors++;

		int errnoBackup = errno;
		fprintf(stderr, "Failed to open dir: %s; Error: %s\n", path.c_str(), strerror(errno) );
		if( (errnoBackup == EACCES) || (errnoBackup == ENOENT) )
			return;

		kill(0, SIGINT);
	}

	/* loop over contents of this entire directory - potentially recursively descending into subdirs
		along the way, depending on config and current global state */
	for ( ; ; )
	{
		errno = 0;
		struct dirent* dirEntry = readdir(dirStream);
		if(!dirEntry)
		{
			if(!errno)
			{
				closedir(dirStream);
				return;
			}

			fprintf(stderr, "Failed to read from dir: %s; Error: %s\n",
				path.c_str(), strerror(errno) );

			statistics.numErrors++;
		}

		if(!strcmp(dirEntry->d_name, ".") || !strcmp(dirEntry->d_name, "..") )
			continue;

		struct stat statBuf;
		int statErrno = -1; // "-1" to let clear that statBuf is not usable yet

		// if dentry type is unknown then we have to stat to know if this is a dir to descend into
		if(config.statAll || dirEntry->d_type == DT_UNKNOWN)
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

			printEntry(entryPath, dirEntry, statErrno ? NULL : &statBuf);

			if(dirDepth < config.maxDirDepth)
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

			printEntry(entryPath, dirEntry, statErrno ? NULL : &statBuf);
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
			"dirs: " << statistics.numDirsFound << std::endl;

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
	std::cout << "                      Hardlinks will not be preserved. Source has to be a dir." << std::endl;
	std::cout << "  --ctime NUM       - ctime filter based on number of days in the past." << std::endl;
	std::cout << "                      +/- prefix to match older or more recent values." << std::endl;
	std::cout << "  --godeep NUM      - Threshold to switch from breadth to depth search." << std::endl;
	std::cout << "                      (Default: number of scan threads)" << std::endl;
	std::cout << "  --json            - Print entries in JSON format. Each file/dir is a" << std::endl;
	std::cout << "                      separate JSON root object. Contained data depends on" << std::endl;
	std::cout << "                      whether \"--" ARG_STAT_LONG "\" is given." << std::endl;
	std::cout << "                      (Hint: Consider the \"jq\" tool to filter results.)" << std::endl;
	std::cout << "  --maxdepth        - Max directory depth to scan. (Path arguments have" << std::endl;
	std::cout << "                      depth 0.)" << std::endl;
	std::cout << "  --mtime NUM       - mtime filter based on number of days in the past." << std::endl;
	std::cout << "                      +/- prefix to match older or more recent values." << std::endl;
	std::cout << "  --name PATTERN    - Filter on filenames (not full path or dirnames)." << std::endl;
	std::cout << "  --nosummary       - Disable summary output to stderr." << std::endl;
	std::cout << "  --path PATTERN    - Filter on path of discovered entries." << std::endl;
	std::cout << "  --print0          - Terminate printed entries with null instead of newline." << std::endl;
	std::cout << "                      (Hint: This is goes nicely with \"xargs -0\".)" << std::endl;
	std::cout << "  --size NUM        - Size filter." << std::endl;
	std::cout << "                      +/- prefix to match greater or smaller values." << std::endl;
	std::cout << "                      Default unit is 512-byte blocks." << std::endl;
	std::cout << "                      'c' suffix to specify bytes instead of 512-byte blocks." << std::endl;
	std::cout << "                      'k'/'M'/'G' suffix for KiB/MiB/GiB units." << std::endl;
	std::cout << "  --stat            - Query attributes of all discovered files & dirs." << std::endl;
	std::cout << "  -t, --threads NUM - Number of scan threads. (Default: 16)" << std::endl;
	std::cout << "  --type TYPE       - Search type. 'f' for regular files, 'd' for directories." << std::endl;
	std::cout << "  --verbose         - Enable verbose output." << std::endl;
	std::cout << "  --version         - Print version and exit." << std::endl;
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
 * Parse commmand line arguments and set corresponding config values.
 */
void parseArguments(int argc, char **argv)
{
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
				{ ARG_FILTER_ATIME, required_argument, 0, 0 },
				{ ARG_FILTER_CTIME, required_argument, 0, 0 },
				{ ARG_FILTER_MTIME, required_argument, 0, 0 },
				{ ARG_FILTER_SIZE, required_argument, 0, 0 },
				{ ARG_GODEEP_LONG, required_argument, 0, 0 },
				{ ARG_HELP_LONG, no_argument, 0, ARG_HELP_SHORT },
				{ ARG_JSON_LONG, no_argument, 0, 0 },
				{ ARG_MAXDEPTH_LONG, required_argument, 0, 0 },
				{ ARG_NAME_LONG, required_argument, 0, 0 },
				{ ARG_NOSUMMARY_LONG, no_argument, 0, 0 },
				{ ARG_PATH_LONG, required_argument, 0, 0 },
				{ ARG_PRINT0_LONG, no_argument, 0, 0 },
				{ ARG_SEARCHTYPE_LONG, required_argument, 0, 0 },
				{ ARG_STAT_LONG, no_argument, 0, 0 },
				{ ARG_THREADS_LONG, required_argument, 0, ARG_THREADS_SHORT },
				{ ARG_VERBOSE_LONG, no_argument, 0, 0 },
				{ ARG_VERSION_LONG, no_argument, 0, 0 },
				{ 0, 0, 0, 0 } // all-zero is the terminating element
		};

		int longOptionIndex = 0;

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

		switch (currentOption)
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
				if(ARG_GODEEP_LONG == currentOptionName)
					config.depthSearchStartThreshold = std::atoi(optarg);
				else
				if(ARG_JSON_LONG == currentOptionName)
					config.printJSON = true;
				else
				if(ARG_MAXDEPTH_LONG == currentOptionName)
					config.maxDirDepth = std::atoi(optarg);
				else
				if(ARG_NAME_LONG == currentOptionName)
					config.nameFilter = optarg;
				else
				if(ARG_NOSUMMARY_LONG == currentOptionName)
					config.printSummary = false;
				else
				if(ARG_PATH_LONG == currentOptionName)
					config.pathFilter = optarg;
				else
				if(ARG_PRINT0_LONG == currentOptionName)
					config.print0 = true;
				else
				if(ARG_SEARCHTYPE_LONG == currentOptionName)
					config.searchType = (strlen(optarg) ? optarg[0] : 0);
				else
				if(ARG_STAT_LONG == currentOptionName)
					config.statAll = true;
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
				exit(1);
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
		exit(1);
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

			fprintf(stderr, "Failed to open dir: %s; Error: %s\n",
				currentPath.c_str(), strerror(errno) );

			retVal = EXIT_FAILURE;

			if( (errnoBackup == EACCES) || (errnoBackup == ENOENT) )
				continue;

			// terminate if error is not just EACCESS or ENOENT
			kill(0, SIGINT);
		}

		if(S_ISDIR(statBuf.st_mode) )
		{ // this entry is a directory
			printEntry(currentPath, NULL, &statBuf);

			if(currentDirDepth < config.maxDirDepth)
			{
				/* mimic gnu findutils behavior to preserve the given number of trailing slashes.
					our scan() func will always add one slash, so we have to remove one here if any */
				std::string currentPathTrimmed(currentPath);
				if(currentPathTrimmed[currentPathTrimmed.length()-1] == '/')
					currentPathTrimmed.erase(currentPathTrimmed.length()-1, 1);

				sharedStack.push(currentPathTrimmed, currentDirDepth + 1);
			}
		}
		else
		{ // this entry is not a directory
			printEntry(currentPath, NULL, &statBuf);
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

