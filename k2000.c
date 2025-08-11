/* vi:set syntax=c expandtab tabstop=4 shiftwidth=4:

 K 2 0 0 0 . C

 Data acquisition using the Keithley 2000 DMM using GPIB.

 Copyright (c) 2004...2025 by Joerg Hau.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License version 2 as
 published by the Free Software Foundation, provided that the copyright
 notice remains intact even in future versions. See the file LICENSE
 for details

 If you use this program (or any part of it) in another application,
 note that the resulting application becomes also GPL. In other
 words, GPL is a "contaminating" license.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 --------------------------------------------------------------------

 Modification/history (adapt VERSION below when changing!):

 2004-08-10    creation based on s7150.c (JHa)
 2004-08-11    more options (JHa)
 2016-10-30    cleaned code (JHa)
 2017-01-06    added modes, updated documentation (JHa)
 2017-01-07    refined details, added subroutines (JHa)
 2017-07-25    added missing '\n' in log file (JHa)
 2025-08-11    moved everything to GitHub (JHa)

 This should compile with any C compiler, something like:

 gcc -Wall -O2 -lgpib k2000.c -o k2000 

 Make sure the user accessing GPIB devices is in group 'gpib'.

*/

#define VERSION "V20170725"	/* String! */

//#define DEBUG  /* diagnostic mode, for development only */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <errno.h>      /* command line reading */
#include <unistd.h>
#include <termios.h>    /* kbhit() */
#include <sys/io.h>
#include <sys/time.h>   /* clock timing */
#include "gpib/ib.h"

#define MAXLEN  127      /* text buffers etc */
#define ESC     27
#define GNUPLOT  "gnuplot"   /* gnuplot executable */

#define ERR_FILE  4         /* error code */
#define ERR_INST  5         /* error code */

/* --- stuff for reading the command line --- */

char *optarg;       /* global: pointer to argument of current option */
int optind = 1;     /* global: index of which argument is next. Is used
                       as a global variable for collection of further
                       arguments (= not options) via argv pointers */

/* --- holds GPIB error code --- */

volatile int iberr;

/* --- stuff for kbhit() ---- */

static struct termios initial_settings, new_settings;
static int peek_character = -1;

void    init_keyboard(void);
void    close_keyboard(void);
int     kbhit(void);
int     readch(void);

/* --- miscellaneous function prototypes ---- */

int     inst_write (const int dvm, const char *cmd);
double  timeinfo (void);
int     strclean (char *buf);
int     GetOpt (int argc, char *argv[], char *optionS);


int main (int argc, char *argv[])
{
static char *disclaimer =
"\nk2000 - Data acquisition using the Keithley 2000 over GPIB. " VERSION ".\n"
"Copyright (C) 2004...2017 by Joerg Hau.\n\n"
"This program is free software; you can redistribute it and/or modify it under\n"
"the terms of the GNU General Public License, version 2, as published by the\n"
"Free Software Foundation.\n\n"
"This program is distributed in the hope that it will be useful, but WITHOUT ANY\n"
"WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A\n"
"PARTICULAR PURPOSE. See the GNU General Public License for details.\n";

static char *msg = "\nSyntax: k2000 [-h] [-a id] [-m mode] [-t dt] [-T timeout] [-d] [-w samp] [-f] [-c \"txt\"] [-g /path/to/gnuplot] [-n] datafile"
"\n        -h       this help screen"
"\n        -a id    use instrument at GPIB address 'id' (default is 16)"
"\n        -m mode  measurement mode (default is 0 for DCV)."
"\n        -t dt    delay between measurements in 0.1 s (default is 10 = 1s)"
"\n        -d       disable instrument display (default is on)"
"\n        -w x     force write to disk every x samples (default is 100)."
"\n        -f       force overwriting of existing file"
"\n        -T min   stop acquisition after this time (in minutes; default 0 = endless)"
"\n        -c txt   comment text"
"\n        -g       specify path/to/gnuplot (if not in your current PATH)"
"\n        -n       no graphics\n\n";

FILE    *outfile, *gp = NULL;
char    inst[MAXLEN], buffer[MAXLEN], filename[MAXLEN], comment[MAXLEN] = "", gnuplot[MAXLEN];
char    do_display = 1, do_graph = 1, do_overwrite = 0;
int     dvm, pad = 16, key, do_flush = 100, delay = 10, mode = 0;
unsigned long loop = 0L;
double  t0, t1;
float   tstop = 0.0;
time_t  t;
static char *scpi_mode[] = {"volt:dc", "curr:dc", "res", "temp", "cont", "diod"};

/* --- gnuplot labels. we could actually query these from the instrument ;-) */
static char *ylabels[]   = {"V", "mA", "Ohm", "degrees C", "Ohm", "mV"}; 

/* --- set the gnuplot executable --- */
sprintf (gnuplot, "%s", GNUPLOT);

/* --- show the usual text --- */
fprintf (stderr, disclaimer);

/* --- decode and read the command line --- */

while ((key = GetOpt(argc, argv, "hfnda:w:t:T:m:c:g:")) != EOF)
    switch (key)
        {
        case 'h':                    /* help me */
            fprintf (stderr, msg);
            return 0;
        case 'f':                    /* force overwriting of existing file */
            do_overwrite = 1;
            continue;
        case 'n':                    /* disable graph display */
            do_graph = 0;
            continue;
        case 'd':                    /* disable display */
            do_display = 0;
            continue;
         case 'c':
            if (strclean (optarg))    
                strcpy (comment, optarg);
            continue;
        case 'g':
            sscanf (optarg, "%80s", gnuplot);
            continue;
        case 'w':
            sscanf (optarg, "%5d", &do_flush);
            continue;
        case 'a':
            sscanf (optarg, "%5d", &pad);
		    if(pad < 0 || pad > 30)
                {
                printf("Error: primary address must be 0...30\n");
                return 1;
                }
            continue;
        case 't':
            sscanf (optarg, "%6d", &delay);
    	    if(delay < 0 || delay > 600)
                {
                printf("Error: delay must be 0...600 (0.1...60 s)\n");
                return 1;
                }
            continue;
        case 'T':
            sscanf (optarg, "%g", &tstop);
            if (tstop < 0.0)
                {
                puts("Error: timeout must be positive.");
                return 1;
                }
            continue;
        case 'm':
            sscanf (optarg, "%5d", &mode);
            if (mode < 0 || mode > 5)
                {
                puts("Error: mode must be 0...5.");
                puts("0 = DCV, 1 = DCA, 2 = Ohm, 3 = Temperature, 4 = Continuity, 5 = Diode");
                return 1;
                }
            continue;
		case '~':                    /* invalid arg */
        default:
		    fprintf (stderr, "'%s -h' for help.\n\n", argv[0]);
            return 1;
        }

if (argv[optind] == NULL)	    /* we need at least one parameter on command line */
    {
    fprintf (stderr, msg);
    fprintf (stderr, "Please specify a data file.\n");
    return 1;
    }

/* --- prepare output data file --- */

strcpy (filename, argv[optind]);
if ((!access(filename, 0)) && (!do_overwrite))	 // If file exists and overwrite is NOT forced
	{
	fprintf (stderr, "\a\nFile '%s' exists - Overwrite? [Y/*] ", filename);
	key = fgetc(stdin);			// read from keyboard
	switch (key)
		{
		case 'Y':
		case 'y':
			break;
        default:
			return 1;
		}
	}
	
if (NULL == (outfile = fopen(filename, "wt")))
    {
    fprintf(stderr, "Could not open '%s' for writing.\n", filename);
    return ERR_FILE;
    }

/* --- now connect to the instrument --- */

dvm = ibdev(0, pad, 0, T1s, 1, 0);
if(dvm < 0)
    {
    fprintf(stderr, "ibdev: error trying to open %i: quit.\n", pad);
    if (do_graph) pclose(gp);
    return ERR_INST;
    }

if (!inst_write (dvm, "*rst;*cls;:form:elem read,unit;*opc"))
    return ERR_INST;

/* Query ID of instrument, save into inst[] */
if (!inst_write (dvm, "*idn?"))     
    return ERR_INST;
if( ibrd(dvm, inst, MAXLEN-1) & ERR)
    {
    fprintf(stderr, "Error reading instrument ID (%d), something is wrong here.\n", ibcnt);
    return ERR_INST;
    }
inst[ibcnt-1] = 0x0;        /* string has CRLF, so remove the LF */

if (!do_display)            /* if blanked, display message */
    {
    if (0 == inst_write (dvm, ":DISP:TEXT:DATA '-ACQUIRING- ';:DISP:TEXT:STAT 1")) 
        return ERR_INST;
    }

/* FIXME: query for any static errors and read result. */ 

/* set mode by copying the relevant string from the pre-defined array */
strcpy (buffer, ":func '");
strcat (buffer, scpi_mode[mode]);
strcat (buffer, "';:init; *opc\n");
#ifdef DEBUG
    fputs(stderr, buffer);
#endif
if (!inst_write (dvm, buffer))
    return ERR_INST;

/* --- prepare gnuplot --- */

if (NULL == (gp = popen("gnuplot","w")))
	{
	fprintf(stderr, "\nCannot launch gnuplot, will continue \"as is\".\n") ;
	fflush(stderr);
	do_graph = 0;	/* do not abort here, just continue */
	};

if (do_graph)	/* prepare gnuplot display defaults */
    {
    fprintf(gp, "set mouse;set mouse labels; set style data lines; set title '%s'\n", filename);
    fprintf(gp, "set grid xt; set grid yt; set xlabel 'min'; set ylabel '%s'\n", ylabels[mode]);
    fflush (gp);
    }

/* --- Set up on-screen display --- */

printf("\n GPIB address :  %d", pad);
printf("\n  Output file :  %s", filename);
if (strlen(comment))
	printf("\n      Comment :  %s", comment);
printf("\n      Refresh :  %d", do_flush);
if (tstop > 0.0)
    printf("\n   Halt after :  %g min", tstop);
printf("\n         Stop :  Press 'q' or ESC.\n");
printf("\n     Count           Time      Reading\n");
fflush(stdout);

/* Get time, write file header */
time(&t);
fprintf(outfile, "# k2000 " VERSION "\n");
fprintf(outfile, "# Instrument: %s\n", inst);
fprintf(outfile, "# %s\n", comment);
fprintf(outfile, "# Acquisition start: %s", ctime(&t));
fprintf(outfile, "# min\treadout\n");
t0 = timeinfo();

init_keyboard();    /* initiate kbhit() functionality */

key = 0;
do  {
    if (delay > 0)
        usleep (delay * 100000.0);

    if (!inst_write (dvm, ":read?"))
        {
        if (gp) 
            pclose(gp);
        fclose (outfile);
        close_keyboard();  
        return ERR_INST;
        }

    if(ibrd(dvm, buffer, 90) & ERR)
    	{
       	fprintf(stderr, "Error trying to read ...\n");
    	break;
       	}
    buffer[ibcnt-1] = 0x0;        /* string has CRLF, so remove the LF */

    if (!strcmp(buffer, "+9.9E37"))
        strcpy(buffer, "OVERFLOW");

    // FIXME: more error checks ?

    t1 = (timeinfo()-t0)/60.0;
    printf("%10lu %10.2f min    %s\r", ++loop, t1, buffer);
    fprintf(outfile, "%.4f\t%s\n", t1, buffer);	// write literally to file
    fflush (stdout);

    /* handle timeout */
    if ((t1 > tstop) && (tstop > 0.0))
        key = ESC;
	
    /* ensure write & display at least every x data points */
    if (!(loop % do_flush))
        {
       	fflush (outfile);
        if (do_graph)
            {
            fprintf(gp, "plot '%s' with lines title ''\n", filename);
            fflush (gp);
            }
        }

    /* look up keyboard for keypress */
    if(kbhit())
        key = readch();
	}
	while ((key != 'q') && (key != ESC));

time(&t);
fprintf(outfile, "# Acquisition stop: %s\n", ctime(&t));
fclose (outfile);
close_keyboard();   /* from kbhit() stuff */

if (do_graph)
    pclose(gp);

if (!do_display)            /* if blanked, display message */
    {
    if (0 == inst_write (dvm, ":DISP:TEXT:STAT 0")) 
        return ERR_INST;
    }

if (!inst_write (dvm, "syst:pres"))     /* Read system ID into *inst */
    return ERR_INST;

printf("\n\n");
return 0;
}


/********************************************************
* inst_write: Writes commnd to instrument.              *
* Input:    - instrument ID                             *
*           - command string                            *
* Return:   1 if OK, 0 if error                         *
********************************************************/
int inst_write (const int dvm, const char *cmd)
{
if (ibwrt(dvm, cmd, strlen(cmd)) & ERR )
    {
    fprintf(stderr, "Error sending '%s': %d\n", cmd, iberr);
    return 0;
    }
return 1;
}


/********************************************************
* TIMEINFO: Returns actual time elapsed since the Epoch *
* Input:    Nothing.                                    *
* Return:   time in microseconds                        *
* Note:     #include <time.h>                           *
*           #include <sys/time.h>                       *
********************************************************/
double timeinfo (void)
{
struct timeval t;

gettimeofday(&t, NULL);
return (double)t.tv_sec + (double)t.tv_usec/1000000.0;
}


/************************************************************************
* Function:     strclean                                                *
* Description:  "cleans" a text buffer obtained by fgets()              *
* Arguments:    Pointer to text buffer                                  *
* Returns:      strlen of buffer                                        *
*************************************************************************/
int strclean (char *buf)
{
int i;

for (i = 0; i < strlen (buf); i++)    /* search for CR/LF */
    {
    if (buf[i] == '\n' || buf[i] == '\r')
        {
        buf[i] = 0;        /* stop at CR or LF */
        break;
        }
    }
return (strlen (buf));
}

/********************************************************
* KBHIT: provides the functionality of DOS's kbhit()    *
* found at http://linux-sxs.org/programming/kbhit.html  *
* Input:    Nothing.                                    *
* Return:   time in microseconds                        *
* Note:     #include <termios.h>                        *
********************************************************/
void init_keyboard (void)
{
tcgetattr( 0, &initial_settings );
new_settings = initial_settings;
new_settings.c_lflag &= ~ICANON;
new_settings.c_lflag &= ~ECHO;
new_settings.c_lflag &= ~ISIG;
new_settings.c_cc[VMIN] = 1;
new_settings.c_cc[VTIME] = 0;
tcsetattr( 0, TCSANOW, &new_settings );
}

void close_keyboard(void)
{
tcsetattr( 0, TCSANOW, &initial_settings );
}

int kbhit (void)
{
char ch;
int nread;

if( peek_character != -1 )
    return( 1 );
new_settings.c_cc[VMIN] = 0;
tcsetattr( 0, TCSANOW, &new_settings );
nread = read( 0, &ch, 1 );
new_settings.c_cc[VMIN] = 1;
tcsetattr( 0, TCSANOW, &new_settings );
if( nread == 1 )
    {
    peek_character = ch;
    return (1);
    }
return (0);
}

int readch (void)
{
char ch;

if( peek_character != -1 )
    {
    ch = peek_character;
    peek_character = -1;
    return( ch );
    }
/* else */
read( 0, &ch, 1 );
return( ch );
}


/***************************************************************************
* GETOPT: Command line parser, system V style.
*
*  Widely (and wildly) adapted from code published by Borland Intl. Inc.
*
*  Note that libc has a function getopt(), however this is not guaranteed
*  to be available for other compilers. Therefore we provide *this* function
*  (which does the same).
*
*  Standard option syntax is:
*
*    option ::= SW [optLetter]* [argLetter space* argument]
*
*  where
*    - SW is '-'
*    - there is no space before any optLetter or argLetter.
*    - opt/arg letters are alphabetic, not punctuation characters.
*    - optLetters, if present, must be matched in optionS.
*    - argLetters, if present, are found in optionS followed by ':'.
*    - argument is any white-space delimited string.  Note that it
*      can include the SW character.
*    - upper and lower case letters are distinct.
*
*  There may be multiple option clusters on a command line, each
*  beginning with a SW, but all must appear before any non-option
*  arguments (arguments not introduced by SW).  Opt/arg letters may
*  be repeated: it is up to the caller to decide if that is an error.
*
*  The character SW appearing alone as the last argument is an error.
*  The lead-in sequence SWSW ("--") causes itself and all the rest
*  of the line to be ignored (allowing non-options which begin
*  with the switch char).
*
*  The string *optionS allows valid opt/arg letters to be recognized.
*  argLetters are followed with ':'.  Getopt () returns the value of
*  the option character found, or EOF if no more options are in the
*  command line. If option is an argLetter then the global optarg is
*  set to point to the argument string (having skipped any white-space).
*
*  The global optind is initially 1 and is always left as the index
*  of the next argument of argv[] which getopt has not taken.  Note
*  that if "--" or "//" are used then optind is stepped to the next
*  argument before getopt() returns EOF.
*
*  If an error occurs, that is an SW char precedes an unknown letter,
*  then getopt() will return a '~' character and normally prints an
*  error message via perror().  If the global variable opterr is set
*  to false (zero) before calling getopt() then the error message is
*  not printed.
*
*  For example, if
*
*    *optionS == "A:F:PuU:wXZ:"
*
*  then 'P', 'u', 'w', and 'X' are option letters and 'A', 'F',
*  'U', 'Z' are followed by arguments. A valid command line may be:
*
*    aCommand  -uPFPi -X -A L someFile
*
*  where:
*    - 'u' and 'P' will be returned as isolated option letters.
*    - 'F' will return with "Pi" as its argument string.
*    - 'X' is an isolated option.
*    - 'A' will return with "L" as its argument.
*    - "someFile" is not an option, and terminates getOpt.  The
*      caller may collect remaining arguments using argv pointers.
***************************************************************************/
int GetOpt (int argc, char *argv[], char *optionS)
{
   static char *letP = NULL;	/* remember next option char's location */
   static char SW = '-';	/* switch character */

   int opterr = 1;		/* allow error message        */
   unsigned char ch;
   char *optP;

   if (argc > optind)
   {
      if (letP == NULL)
      {
	 if ((letP = argv[optind]) == NULL || *(letP++) != SW)
	    goto gopEOF;

	 if (*letP == SW)
	 {
	    optind++;
	    goto gopEOF;
	 }
      }
      if (0 == (ch = *(letP++)))
      {
	 optind++;
	 goto gopEOF;
      }
      if (':' == ch || (optP = strchr (optionS, ch)) == NULL)
	 goto gopError;
      if (':' == *(++optP))
      {
	 optind++;
	 if (0 == *letP)
	 {
	    if (argc <= optind)
	       goto gopError;
	    letP = argv[optind++];
	 }
	 optarg = letP;
	 letP = NULL;
      }
      else
      {
	 if (0 == *letP)
	 {
	    optind++;
	    letP = NULL;
	 }
	 optarg = NULL;
      }
      return ch;
   }

 gopEOF:
   optarg = letP = NULL;
   return EOF;

 gopError:
   optarg = NULL;
   errno = EINVAL;
   if (opterr)
      perror ("\nCommand line option");
   return ('~');
}

