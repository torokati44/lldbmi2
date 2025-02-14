
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <signal.h>
#ifdef __APPLE__
#include <util.h>
#endif
#include <sys/stat.h>
#include <sys/timeb.h>
#include <stdarg.h>
#include <string.h>
#include <iostream>


#include <string.h>
#include <termios.h>
#include "linenoise.h"


#include <lldb/API/SBDebugger.h>

#include "lldbmi2.h"
#include "engine.h"
#include "variables.h"
#include "log.h"
#include "test.h"
#include "version.h"


void help (STATE *pstate)
{
	fprintf (stderr, "%s\n", pstate->lldbmi2Prompt);
	fprintf (stderr, "Description:\n");
	fprintf (stderr, "   A MI2 interface to LLDB\n");
	fprintf (stderr, "Authors:\n");
	fprintf (stderr, "   Didier Bertrand, 2015, 2016, 2018\n");
	fprintf (stderr, "   Eduard Matveev, 2016\n");
	fprintf (stderr, "   David Jenkins, 2018\n");
	fprintf (stderr, "Syntax:\n");
	fprintf (stderr, "   lldbmi2 --version [options]\n");
	fprintf (stderr, "   lldbmi2 --interpreter mi2 [options]\n");
	fprintf (stderr, "Arguments:\n");
	fprintf (stderr, "   --version:           Return GDB's version (GDB 7.12.1) and exits.\n");
	fprintf (stderr, "   --interpreter mi2:   Standard mi2 interface.\n");
	fprintf (stderr, "   --interpreter=mi2:   Standard mi2 interface.\n");
	fprintf (stderr, "Options:\n");
	fprintf (stderr, "   --log:                Create log file in project root directory.\n");
	fprintf (stderr, "   --logmask mask:       Select log categories. 0xFFF. See source code for values.\n");
	fprintf (stderr, "   --arch arch_name:     Force a different architecture from host architecture: arm64, x86_64, i386\n");
	fprintf (stderr, "   --test n:             Execute test sequence (to debug lldmi2).\n");
	fprintf (stderr, "   --script file_path:   Execute test script or replay logfile (to debug lldmi2).\n");
	fprintf (stderr, "   --nx:                 Ignored.\n");
	fprintf (stderr, "   --frames frames:      Max number of frames to display (%d).\n", FRAMES_MAX);
	fprintf (stderr, "   --children children:  Max number of children to check for update (%d).\n", CHILDREN_MAX);
	fprintf (stderr, "   --walkdepth depth:    Max walk depth in search for variables (%d).\n", WALK_DEPTH_MAX);
	fprintf (stderr, "   --changedepth depth:  Max depth to check for updated variables (%d).\n", CHANGE_DEPTH_MAX);
}


LIMITS limits;
static STATE state;

int
main (int argc, char **argv, char **envp)
{
	int narg;
	char commandLine[BIG_LINE_MAX];		// data from cdt
	char consoleLine[LINE_MAX];			// data from eclipse's console
	long chars;
	int isVersion=0, isInterpreter=0;
	const char *testCommand=NULL;
	int  isLog=0;
	unsigned int logmask=LOG_DEV;

	state.ptyfd = EOF;
	state.cdtptyfd = EOF;
	state.gdbPrompt = "GNU gdb (GDB) 7.12.1";
	snprintf (state.lldbmi2Prompt, NAME_MAX, "lldbmi2 version %s", LLDBMI2_VERSION);
	state.cdtbufferB.grow(BIG_LINE_MAX);

	limits.frames_max = FRAMES_MAX;
	limits.children_max = CHILDREN_MAX;
	limits.walk_depth_max = WALK_DEPTH_MAX;
	limits.change_depth_max = CHANGE_DEPTH_MAX;

	// create a log filename from program name and open log file
	if (true) {
		setlogfile (state.logfilename, sizeof(state.logfilename), argv[0], "lldbmi2.log");
		openlogfile (state.logfilename);
		setlogmask (logmask);
	}

	// get args
	for (narg=0; narg<argc; narg++) {
		logarg (argv[narg]);
		if (strcmp (argv[narg],"--version") == 0)
			isVersion = 1;
		else if (strcmp (argv[narg],"--interpreter") == 0) {
			isInterpreter = 1;
			if (++narg<argc)
				logarg(argv[narg]);
		}
		else if (strcmp (argv[narg],"--interpreter=mi2") == 0)
			isInterpreter = 1;
		else if ((strcmp (argv[narg],"-i") == 0) && (strcmp (argv[narg+1], "mi") == 0))
			isInterpreter = 1;
		else if (strcmp (argv[narg],"--arch") == 0 ) {
			if (++narg<argc)
				strcpy (state.arch, logarg(argv[narg]));
		}
		else if (strcmp (argv[narg],"--test") == 0 ) {
			limits.istest = true;
			if (++narg<argc)
				sscanf (logarg(argv[narg]), "%d", &state.test_sequence);
			if (state.test_sequence)
				setTestSequence (state.test_sequence);
		}
		else if (strcmp (argv[narg],"--script") == 0 ) {
			limits.istest = true;
			if (++narg<argc)
				strcpy (state.test_script, logarg(argv[narg]));		// no spaces allowed in the name
			if (state.test_script[0])
				setTestScript (state.test_script);
		}
		else if (strcmp (argv[narg],"--log") == 0 )
			isLog = 1;
		else if (strcmp (argv[narg],"--logmask") == 0 ) {
			isLog = 1;
			if (++narg<argc)
				sscanf (logarg(argv[narg]), "%x", &logmask);
		}
		else if (strcmp (argv[narg],"--frames") == 0 ) {
			if (++narg<argc)
				sscanf (logarg(argv[narg]), "%d", &limits.frames_max);
		}
		else if (strcmp (argv[narg],"--children") == 0 ) {
			if (++narg<argc)
				sscanf (logarg(argv[narg]), "%d", &limits.children_max);
		}
		else if (strcmp (argv[narg],"--walkdepth") == 0 ) {
			if (++narg<argc)
				sscanf (logarg(argv[narg]), "%d", &limits.walk_depth_max);
		}
		else if (strcmp (argv[narg],"--changedepth") == 0 ) {
			if (++narg<argc)
				sscanf (logarg(argv[narg]), "%d", &limits.change_depth_max);
		}
		else if (strcmp (argv[narg],"-ex") == 0) {
			if (++narg<argc) {
				if (strncmp(argv[narg], "new-ui", strlen("new-ui")) == 0) {
					sscanf(argv[narg], "new-ui mi %s", state.cdtptyname);
					logprintf (LOG_INFO, "pty %s\n", state.cdtptyname);
					
					state.cdtptyfd = open (state.cdtptyname, O_RDWR);

					// set pty in raw mode
					struct termios t;
					if (tcgetattr(state.cdtptyfd, &t) != -1) {
						logprintf (LOG_INFO, "setting pty\n");
						// Noncanonical mode, disable signals, extended input processing, and echoing
						t.c_lflag &= ~(ICANON | ISIG | IEXTEN | ECHO);
						// Disable special handling of CR, NL, and BREAK.
						// No 8th-bit stripping or parity error handling
						// Disable START/STOP output flow control
						t.c_iflag &= ~(BRKINT | ICRNL | IGNBRK | IGNCR | INLCR |
								INPCK | ISTRIP | IXON | PARMRK);
						// Disable all output processing
						t.c_oflag &= ~OPOST;
						t.c_cc[VMIN] = 1;		// Character-at-a-time input
						t.c_cc[VTIME] = 0;		// with blocking
						int ret = tcsetattr(state.cdtptyfd, TCSAFLUSH, &t);
						logprintf (LOG_INFO, "setting pty %d\n", ret);
					}
				}
			}
		}
	}

	// log program args
	addlog("\n");
	logprintf (LOG_ARGS, NULL);

	state.envp[0] = NULL;
	state.envpentries = 0;
	state.envspointer = state.envs;
	const char *wl = "PWD=";		// want to get eclipse project_loc if any
	int wll = strlen(wl);
	// copy environment for tested program
	for (int ienv=0; envp[ienv]; ienv++) {
		addEnvironment (&state, envp[ienv]);
		if (strncmp(envp[ienv], wl, wll)==0)
			strcpy (state.project_loc, envp[ienv]+wll);
	}

	// return gdb version if --version
	if (isVersion) {
		cdtprintf ("%s, %s, %s\n", state.gdbPrompt, state.lldbmi2Prompt, SBDebugger::GetVersionString());
		return EXIT_SUCCESS;
	}
	// check if --interpreter mi2
	else if (!isInterpreter) {
		help (&state);
		return EXIT_FAILURE;
	}
	
	initializeSB (&state);
	signal (SIGINT, signalHandler);
	//signal (SIGSTOP, signalHandler);

	logprintf (LOG_TRACE, "printing prompt\n");
	sleep(2);
	cdtprintf ("(gdb)\n");
	const char *prompt = state.debugger.GetPrompt();

		
		 /* Asynchronous mode using the multiplexing API: wait for
        * data on stdin, and simulate async data coming from some source
        * using the select(2) timeout. */
        struct linenoiseState ls;
        char buf[1024];
        char *line = linenoiseEditMore;
        linenoiseEditStart(&ls,-1,-1,buf,sizeof(buf), prompt);

	// main loop
	while (!state.eof) {
		if (limits.istest)
			logprintf (LOG_NONE, "main loop\n");
		
		
		

		fd_set readfds;
		FD_ZERO(&readfds);

		struct timeval tv;
		tv.tv_sec = 0; // 0.1 sec timeout
		tv.tv_usec = 100000;
		
	
		int nfds = STDIN_FILENO+1;
		// check command from CDT
		FD_SET (STDIN_FILENO, &readfds);
		if (state.cdtptyfd != EOF) {
			// check data from Eclipse's console
			FD_SET (state.cdtptyfd, &readfds);
			logprintf (LOG_TRACE, "select from both\n");
			nfds = std::max(nfds, state.cdtptyfd+1);
		}
		
		if (state.ptyfd != EOF) {
			// check data from Eclipse's console
			FD_SET (state.ptyfd, &readfds);
			logprintf (LOG_TRACE, "select from both\n");
			nfds = std::max(nfds, state.ptyfd+1);
		}
		
		int retval = select(nfds, &readfds, NULL, NULL, &tv);
		
		
		if (FD_ISSET(STDIN_FILENO, &readfds) && !state.eof) {
			line = linenoiseEditFeed(&ls);
			/* A NULL return means: line editing is continuing.
			* Otherwise the user hit enter or stopped editing
			* (CTRL+C/D). */
			if (line != linenoiseEditMore) {
				
				linenoiseEditStop(&ls);
				
				logprintf (LOG_TRACE, "read in\n");
				chars = strlen(line);
				logprintf (LOG_TRACE, "read out %d chars\n", chars);
				if (chars>0) {
					SBCommandInterpreter interp = state.debugger.GetCommandInterpreter();
					SBCommandReturnObject result;
					ReturnStatus retcode = interp.HandleCommand(line, result);
					
					write(STDOUT_FILENO, result.GetOutput(), result.GetOutputSize());
					write(STDERR_FILENO, result.GetError(), result.GetErrorSize());			
				}
				else
					state.eof = true;
				
				free(line);
				linenoiseEditStart(&ls,-1,-1,buf,sizeof(buf), prompt);

			}
		}
        
        if (line == NULL) exit(0); /* Ctrl+D/C. */
		
		
	
		if (state.cdtptyfd!=EOF) {			// input from user to program
			if (FD_ISSET(state.cdtptyfd, &readfds) && !state.eof && !limits.istest) {
				logprintf (LOG_TRACE, "cdt pty read in\n");
				chars = read (state.cdtptyfd, consoleLine, sizeof(consoleLine)-1);
				logprintf (LOG_TRACE, "cdt pty read out %d chars\n", chars);
				if (chars>0) {
					logprintf (LOG_PROG_OUT, "cdt pty read %d chars: '%s'\n", chars, consoleLine);
					consoleLine[chars] = '\0';
					
				while (fromCDT (&state,consoleLine,sizeof(consoleLine)) == MORE_DATA)
					consoleLine[0] = '\0';
				}
			}
		}
		
		if (state.ptyfd!=EOF) {			// input from user to program
			if (FD_ISSET(state.ptyfd, &readfds) && !state.eof && !limits.istest) {
				logprintf (LOG_TRACE, "pty read in\n");
				chars = read (state.ptyfd, consoleLine, sizeof(consoleLine)-1);
				logprintf (LOG_TRACE, "pty read out %d chars\n", chars);
				if (chars>0) {
					logprintf (LOG_PROG_OUT, "pty read %d chars: '%s'\n", chars, consoleLine);
					consoleLine[chars] = '\0';
				}
			}
		}
		
		
		// execute test command if test mode
		if (!state.eof && limits.istest && !state.isrunning) {
			if ((testCommand=getTestCommand ())!=NULL) {
				snprintf (commandLine, sizeof(commandLine), "%s\n", testCommand);
				fromCDT (&state, commandLine, sizeof(commandLine));
			}
		}
		// execute stacked commands if many command arrived once
		if (!state.eof && state.cdtbufferB.size()>0) {
			commandLine[0] = '\0';
			while (fromCDT (&state, commandLine, sizeof(commandLine)) == MORE_DATA)
				;
		}
	}

	if (state.ptyfd != EOF)
		close (state.ptyfd);
	terminateSB ();

	logprintf (LOG_INFO, "main exit\n");
	closelogfile ();

	return EXIT_SUCCESS;
}

// log an argument and return the argument
const char *
logarg (const char *arg) {
	addlog (arg);
	addlog (" ");
	return arg;
}

void
writetocdt (const char *line)
{
	logprintf (LOG_TRACE, "writetocdt '%s'\n", line);
	logdata (LOG_CDT_OUT, line, strlen(line));
	writelog (state.cdtptyfd > 0 ? state.cdtptyfd : STDOUT_FILENO, line, strlen(line));
}

void
cdtprintf ( const char *format, ... )
{
	logprintf (LOG_NONE, "cdtprintf (...)\n");
	static StringB buffer(BIG_LINE_MAX);
	va_list args;

	if (format!=NULL) {
		va_start (args, format);
		buffer.vosprintf (0, format, args);
		va_end (args);
		if ((buffer.c_str()[0] == '0') && (buffer.c_str()[1] == '^'))
			buffer.clear(1,0);
		writetocdt (buffer.c_str());
	}
}

std::string ReplaceString(std::string subject, const std::string& search,
                          const std::string& replace) {
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != std::string::npos) {
         subject.replace(pos, search.length(), replace);
         pos += replace.length();
    }
    return subject;
}

void
strrecprintf (const char *typestr, const char *format, va_list args)
{
	logprintf (LOG_NONE, "srcprintf (...)\n");
	static StringB buffer(BIG_LINE_MAX);
	static StringB lineout(BIG_LINE_MAX);
	static StringB prepend(10);

	buffer.vosprintf (0, format, args);
	prepend.clear();
	prepend.append(typestr);
	prepend.append("\"");
	lineout.clear();
	lineout.append(prepend.c_str());
	for (int ndx=0; ndx<buffer.size(); ndx++) {
		if (buffer.c_str()[ndx] == '\"')
			lineout.append("\\\"");
		else if (buffer.c_str()[ndx] == '\n') {
			lineout.append("\\n\"\n");
			writetocdt(lineout.c_str());
			lineout.clear();
			lineout.append(prepend.c_str());
		}
		else 
			lineout.append(buffer.c_str()[ndx]);
	}
	if (lineout.size() > 2) {
		lineout.append("\n\"");
		writetocdt (lineout.c_str());
	}
}

void
srcprintf (const char *format, ... )
{
	va_list args;

	if (format!=NULL) {
		va_start (args, format);
		strrecprintf("&", format, args);
		va_end (args);
	}
}

void
srlprintf (const char *format, ... )
{
	va_list args;

	if (format!=NULL) {
		va_start (args, format);
		strrecprintf("~", format, args);
		va_end (args);
	}
}

static int signals_received=0;

void
signalHandler (int signo)
{
	logprintf (LOG_TRACE, "signalHandler (%d)\n", signo);
	if (signo==SIGINT)
		logprintf (LOG_INFO, "signal SIGINT\n");
	else
		logprintf (LOG_INFO, "signal %s\n", signo);
	if (signo==SIGINT) {
		if (state.process.IsValid() && signals_received==0) {
			int selfPID = getpid();
			int processPID = state.process.GetProcessID();
			logprintf (LOG_INFO, "signal_handler: signal SIGINT. self PID = %d, process pid = %d\n", selfPID, processPID);
			logprintf (LOG_INFO, "send signal SIGSTOP to process %d\n", processPID);
		//	state.process.Signal (SIGSTOP);
			logprintf (LOG_INFO, "Stop process\n");
			state.process.Stop();
		//	++signals_received;
		}
		else
			state.debugger.DispatchInputInterrupt();
	}
}

/*
BUTTON PAUSE (SIGSTOP or ^Z)
173728.927 ---  signal SIGINT
173728.927 ---  signal_handler: signal SIGINT. self PID = 15659, process pid = 15660
173728.927 ---  send signal SIGSTOP to process 15660
173728.930 ###  eStateStopped
173728.930 <<=  |=thread-created,id="2",group-id="i1"\n|
173728.931 <<=  |*stopped,reason="signal-received",signal-name="SIGSTOP",frame={addr="0x000000000001710a",func="__semwait_signal",args=[],file="libsystem_kernel.dylib"}thread-id="1",stopped-threads="all"\n(gdb)\n|
173728.939 >>=  |32thread|
173728.939 !!!  command not understood: 173728.939   |thread|
173728.940 <<=  |32^error,msg="Command unimplemented."\n(gdb)\n|

BUTTON STOP (SIGINT or ^C)
173504.979 ###  eBroadcastBitSTDOUT
173504.979 <<<  |loop 0\n|
173505.222 ---  signal SIGINT
173505.222 ---  signal_handler: signal SIGINT. self PID = 15615, process pid = 15616
173505.222 ---  send signal SIGSTOP to process 15616
173505.223 >>=  |32-interpreter-exec --thread-group i1 console kill|
173505.223 ---  console kill: send SIGINT
173505.223 <<=  |32^done\n(gdb)\n|
173505.233 ###  eStateStopped
173505.233 <<=  |=thread-created,id="2",group-id="i1"\n|

005910.640 <<=  |*stopped,reason="breakpoint-hit",disp="keep",bkptno="1",frame={addr="0x000000000000127a",func="waitthread()",args=[],file="tests.cpp",fullname="/Users/didier/Projets/git-lldbmi2/lldbmi2/tests/src/tests.cpp",line="50"},thread-id="1",stopped-threads="all"\n(gdb)\n|
005910.722 <<=  |30^done,groups=[{id="i1",type="process",pid="20408",executable="/Users/didier/Projets/git-lldbmi2/lldbmi2/build/tests"}]\n(gdb)\n|

005610.309 <<=  |*stopped,reason="signal-received",signal-name="SIGSTOP",frame={addr="0x0000000000001286",func="waitthread()",args=[],file="tests.cpp",fullname="/Users/didier/Projets/git-lldbmi2/lldbmi2/tests/src/tests.cpp",line="50"},thread-id="1",stopped-threads="all"\n(gdb)\n|
005610.319 <<=  |30^done,groups=[{id="i1",type="process",pid="20359",executable="/Users/didier/Projets/git-lldbmi2/lldbmi2/build/tests"}]\n(gdb)\n|

 */
