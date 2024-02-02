
#ifndef LLDBMIG_H
#define LLDBMIG_H

#include <lldb/API/LLDB.h>
using namespace lldb;

// use system constants
// PATH_MAX = 1024
// LINE_MAX = 2048
// NAME_MAX = 255
#ifdef WIN32
    #include <winsock.h>
    #define NAME_MAX 255
    #define LINE_MAX 2048
    /* Create a realpath replacement macro for when compiling under mingw
    * Based upon https://stackoverflow.com/questions/45124869/cross-platform-alternative-to-this-realpath-definition
    */
    #define realpath(N,R) _fullpath((R),(N),PATH_MAX)
#endif

#ifdef __APPLE__
#include <sys/syslimits.h>
#else
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include "strlxxx.h"
#endif
#include "stringb.h"

#define WAIT_DATA  0
#define MORE_DATA  1

#define FALSE 0
#define TRUE  1

#define THREADS_MAX 50
#define FRAMES_MAX  75

#define VALUE_MAX (NAME_MAX<<1)
#define BIG_VALUE_MAX (NAME_MAX<<3)
#define BIG_LINE_MAX (LINE_MAX<<3)

#define ENV_ENTRIES 200

// static context
typedef struct {
	bool istest;
	int frames_max;
	int children_max;
	int walk_depth_max;
	int change_depth_max;
} LIMITS;


// dynamic context
typedef struct {
	int  ptyfd = -1;
	int  cdtptyfd = -1;
	bool eof;
	bool procstop;
	bool isrunning;
	bool wanttokill;
	char arch[NAME_MAX];
	int test_sequence;
	char test_script[PATH_MAX];
	const char *envp[ENV_ENTRIES];
	int  envpentries;
	char envs[BIG_LINE_MAX];
	char *envspointer;
	char project_loc[PATH_MAX];
	StringB cdtbufferB;
	char cdtptyname[NAME_MAX];
	char logfilename[PATH_MAX];
	const char *gdbPrompt;
	char lldbmi2Prompt[NAME_MAX];
	char threadgroup[NAME_MAX];
	SBDebugger debugger;
	SBProcess process;
	SBListener listener;
	int threadids[THREADS_MAX];
} STATE;

const char * logarg (const char *arg);
void         writetocdt    (const char *line);
void         cdtprintf     (const char *format, ... );
void         srcprintf     (const char *format, ... );
void         srlprintf     (const char *format, ... );
void         signalHandler (int vSigno);

#endif	// LLDBMIG_H
