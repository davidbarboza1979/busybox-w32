/* vi: set sw=4 ts=4: */
/*
 * Termios command line History and Editing.
 *
 * Copyright (c) 1986-2003 may safely be consumed by a BSD or GPL license.
 * Written by:   Vladimir Oleynik <dzo@simtreas.ru>
 *
 * Used ideas:
 *      Adam Rogoyski    <rogoyski@cs.utexas.edu>
 *      Dave Cinege      <dcinege@psychosis.com>
 *      Jakub Jelinek (c) 1995
 *      Erik Andersen    <andersen@codepoet.org> (Majorly adjusted for busybox)
 *
 * This code is 'as is' with no warranty.
 */

/*
   Usage and known bugs:
   Terminal key codes are not extensive, and more will probably
   need to be added. This version was created on Debian GNU/Linux 2.x.
   Delete, Backspace, Home, End, and the arrow keys were tested
   to work in an Xterm and console. Ctrl-A also works as Home.
   Ctrl-E also works as End.

   Small bugs (simple effect):
   - not true viewing if terminal size (x*y symbols) less
     size (prompt + editor`s line + 2 symbols)
   - not true viewing if length prompt less terminal width
 */

/*
CONFIG_FEATURE_COMMAND_EDITING=y
# CONFIG_FEATURE_COMMAND_EDITING_VI is not set
CONFIG_FEATURE_COMMAND_HISTORY=15
# CONFIG_FEATURE_COMMAND_SAVEHISTORY is not set
# CONFIG_FEATURE_COMMAND_TAB_COMPLETION is not set
# CONFIG_FEATURE_COMMAND_USERNAME_COMPLETION is not set
# CONFIG_FEATURE_SH_FANCY_PROMPT is not set
Sizes with the above:
# size cmdedit.o
   text    data     bss     dec     hex filename
   2374       4     228    2606     a2e cmdedit.o
# nm --size-sort cmdedit.o
00000004 b cmdedit_prmt_len
00000004 b cmdedit_prompt
00000004 d cmdedit_termw
00000004 b cmdedit_x
00000004 b cmdedit_y
00000004 b command_ps
00000004 b cur_history
00000004 b cursor
00000004 b handlers_sets
00000004 b len
00000004 b n_history
00000004 b previous_SIGWINCH_handler
00000009 t beep
00000017 t goto_new_line
0000001a t input_end
0000001b t input_backspace
0000001e t input_forward
00000027 t get_next_history
00000036 t put_prompt
00000039 t redraw
0000003c b initial_settings
0000003c b new_settings
00000040 b history
00000047 t input_delete
0000004d t get_previous_history
00000059 t cmdedit_reset_term
0000006c t cmdedit_set_out_char
00000087 t input_backward
000000a1 t win_changed
0000053c T cmdedit_read_input
*/

#include <sys/ioctl.h>
#include "busybox.h"
#include "cmdedit.h"


/* FIXME: obsolete CONFIG item? */
#define ENABLE_FEATURE_NONPRINTABLE_INVERSE_PUT 0


#ifdef TEST

#define ENABLE_FEATURE_COMMAND_EDITING 0
#define ENABLE_FEATURE_COMMAND_TAB_COMPLETION 0
#define ENABLE_FEATURE_COMMAND_USERNAME_COMPLETION 0
#define ENABLE_FEATURE_NONPRINTABLE_INVERSE_PUT 0
#define ENABLE_FEATURE_CLEAN_UP 0

#endif  /* TEST */


/* Entire file (except TESTing part) sits inside this #if */
#if ENABLE_FEATURE_COMMAND_EDITING


#if ENABLE_LOCALE_SUPPORT
#define Isprint(c) isprint(c)
#else
#define Isprint(c) ((c) >= ' ' && (c) != ((unsigned char)'\233'))
#endif

#define ENABLE_FEATURE_GETUSERNAME_AND_HOMEDIR \
(ENABLE_FEATURE_COMMAND_USERNAME_COMPLETION || ENABLE_FEATURE_SH_FANCY_PROMPT)

/* Maximum length of command line history */
#if !ENABLE_FEATURE_COMMAND_HISTORY
#define MAX_HISTORY   15
#else
#define MAX_HISTORY   (CONFIG_FEATURE_COMMAND_HISTORY + 0)
#endif


/* Current termios and the previous termios before starting sh */
static struct termios initial_settings, new_settings;

static
volatile unsigned cmdedit_termw = 80;        /* actual terminal width */
static
volatile int handlers_sets = 0; /* Set next bits: */
enum {
	SET_ATEXIT = 1,         /* when atexit() has been called
				   and get euid,uid,gid to fast compare */
	SET_WCHG_HANDLERS = 2,  /* winchg signal handler */
	SET_RESET_TERM = 4,     /* if the terminal needs to be reset upon exit */
};


static int cmdedit_x;           /* real x terminal position */
static int cmdedit_y;           /* pseudoreal y terminal position */
static int cmdedit_prmt_len;    /* length of prompt (without colors etc) */

static int cursor;
static int len;
static char *command_ps;
static SKIP_FEATURE_SH_FANCY_PROMPT(const) char *cmdedit_prompt;

#if ENABLE_FEATURE_SH_FANCY_PROMPT
static char *hostname_buf;
static int num_ok_lines = 1;
#endif

#if ENABLE_FEATURE_GETUSERNAME_AND_HOMEDIR
static char *user_buf = "";
static char *home_pwd_buf = "";
#endif

#if ENABLE_FEATURE_GETUSERNAME_AND_HOMEDIR || ENABLE_FEATURE_COMMAND_TAB_COMPLETION
static int my_euid;
#endif

#if ENABLE_FEATURE_COMMAND_TAB_COMPLETION
static int my_uid;
static int my_gid;
#endif

/* Put 'command_ps[cursor]', cursor++.
 * Advance cursor on screen. If we reached right margin, scroll text up
 * and remove terminal margin effect by printing 'next_char' */
static void cmdedit_set_out_char(int next_char)
{
	int c = (unsigned char)command_ps[cursor];

	if (c == '\0') {
		/* erase character after end of input string */
		c = ' ';
	}
#if ENABLE_FEATURE_NONPRINTABLE_INVERSE_PUT
	/* Display non-printable characters in reverse */
	if (!Isprint(c)) {
		if (c >= 128)
			c -= 128;
		if (c < ' ')
			c += '@';
		if (c == 127)
			c = '?';
		printf("\033[7m%c\033[0m", c);
	} else
#endif
	{
		if (initial_settings.c_lflag & ECHO)
			putchar(c);
	}
	if (++cmdedit_x >= cmdedit_termw) {
		/* terminal is scrolled down */
		cmdedit_y++;
		cmdedit_x = 0;
		/* destroy "(auto)margin" */
		putchar(next_char);
		putchar('\b');
	}
// Huh? What if command_ps[cursor] == '\0' (we are at the end already?)
	cursor++;
}

/* Move to end of line (by printing all chars till the end) */
static void input_end(void)
{
	while (cursor < len)
		cmdedit_set_out_char(' ');
}

/* Go to the next line */
static void goto_new_line(void)
{
	input_end();
	if (cmdedit_x)
		putchar('\n');
}


static void out1str(const char *s)
{
	if (s)
		fputs(s, stdout);
}

static void beep(void)
{
	putchar('\007');
}

/* Move back one character */
/* (optimized for slow terminals) */
static void input_backward(unsigned num)
{
	int count_y;

	if (num > cursor)
		num = cursor;
	if (!num)
		return;
	cursor -= num;

	if (cmdedit_x >= num) {
		cmdedit_x -= num;
		if (num <= 4) {
			do putchar('\b'); while (--num);
			return;
		}
		printf("\033[%uD", num);
		return;
	}

	/* Need to go one or more lines up */
	num -= cmdedit_x;
	count_y = 1 + (num / cmdedit_termw);
	cmdedit_y -= count_y;
	cmdedit_x = cmdedit_termw * count_y - num;
	/* go to 1st col; go up; go to correct column */
	printf("\r" "\033[%dA" "\033[%dC", count_y, cmdedit_x);
}

static void put_prompt(void)
{
	out1str(cmdedit_prompt);
	cmdedit_x = cmdedit_prmt_len;   /* count real x terminal position */
	cursor = 0;
// Huh? what if cmdedit_prmt_len >= width?
	cmdedit_y = 0;                  /* new quasireal y */
}

/* draw prompt, editor line, and clear tail */
static void redraw(int y, int back_cursor)
{
	if (y > 0)                              /* up to start y */
		printf("\033[%dA", y);
	putchar('\r');
	put_prompt();
	input_end();                            /* rewrite */
	printf("\033[J");                       /* erase after cursor */
	input_backward(back_cursor);
}

#if ENABLE_FEATURE_COMMAND_EDITING_VI
#define DELBUFSIZ 128
static char *delbuf;  /* a (malloced) place to store deleted characters */
static char *delp;
static char newdelflag;      /* whether delbuf should be reused yet */
#endif

/* Delete the char in front of the cursor, optionally saving it
 * for later putback */
static void input_delete(int save)
{
	int j = cursor;

	if (j == len)
		return;

#if ENABLE_FEATURE_COMMAND_EDITING_VI
	if (save) {
		if (newdelflag) {
			if (!delbuf)
				delbuf = malloc(DELBUFSIZ);
			/* safe if malloc fails */
			delp = delbuf;
			newdelflag = 0;
		}
		if (delbuf && (delp - delbuf < DELBUFSIZ))
			*delp++ = command_ps[j];
	}
#endif

	strcpy(command_ps + j, command_ps + j + 1);
	len--;
	input_end();                    /* rewrite new line */
	cmdedit_set_out_char(' ');      /* erase char */
	input_backward(cursor - j);     /* back to old pos cursor */
}

#if ENABLE_FEATURE_COMMAND_EDITING_VI
static void put(void)
{
	int ocursor;
	int j = delp - delbuf;

	if (j == 0)
		return;
	ocursor = cursor;
	/* open hole and then fill it */
	memmove(command_ps + cursor + j, command_ps + cursor, len - cursor + 1);
	strncpy(command_ps + cursor, delbuf, j);
	len += j;
	input_end();                    /* rewrite new line */
	input_backward(cursor - ocursor - j + 1); /* at end of new text */
}
#endif

/* Delete the char in back of the cursor */
static void input_backspace(void)
{
	if (cursor > 0) {
		input_backward(1);
		input_delete(0);
	}
}


/* Move forward one character */
static void input_forward(void)
{
	if (cursor < len)
		cmdedit_set_out_char(command_ps[cursor + 1]);
}

#if ENABLE_FEATURE_COMMAND_TAB_COMPLETION

static char **matches;
static int num_matches;

static void add_match(char *matched)
{
	int nm = num_matches;
	int nm1 = nm + 1;

	matches = xrealloc(matches, nm1 * sizeof(char *));
	matches[nm] = matched;
	num_matches++;
}

#if ENABLE_FEATURE_COMMAND_USERNAME_COMPLETION

static void username_tab_completion(char *ud, char *with_shash_flg)
{
	struct passwd *entry;
	int userlen;

	ud++;                           /* ~user/... to user/... */
	userlen = strlen(ud);

	if (with_shash_flg) {           /* "~/..." or "~user/..." */
		char *sav_ud = ud - 1;
		char *home = 0;
		char *temp;

		if (*ud == '/') {       /* "~/..."     */
			home = home_pwd_buf;
		} else {
			/* "~user/..." */
			temp = strchr(ud, '/');
			*temp = 0;              /* ~user\0 */
			entry = getpwnam(ud);
			*temp = '/';            /* restore ~user/... */
			ud = temp;
			if (entry)
				home = entry->pw_dir;
		}
		if (home) {
			if ((userlen + strlen(home) + 1) < BUFSIZ) {
				char temp2[BUFSIZ];     /* argument size */

				/* /home/user/... */
				sprintf(temp2, "%s%s", home, ud);
				strcpy(sav_ud, temp2);
			}
		}
	} else {
		/* "~[^/]*" */
		setpwent();

		while ((entry = getpwent()) != NULL) {
			/* Null usernames should result in all users as possible completions. */
			if ( /*!userlen || */ !strncmp(ud, entry->pw_name, userlen)) {
				add_match(xasprintf("~%s/", entry->pw_name));
			}
		}

		endpwent();
	}
}
#endif  /* FEATURE_COMMAND_USERNAME_COMPLETION */

enum {
	FIND_EXE_ONLY = 0,
	FIND_DIR_ONLY = 1,
	FIND_FILE_ONLY = 2,
};

#if ENABLE_ASH
const char *cmdedit_path_lookup;
#endif
static int path_parse(char ***p, int flags)
{
	int npth;
	const char *tmp;
#if ENABLE_ASH
	const char *pth = cmdedit_path_lookup;
#else
	const char *pth = getenv("PATH")
#endif

	/* if not setenv PATH variable, to search cur dir "." */
	if (flags != FIND_EXE_ONLY)
		return 1;
	/* PATH=<empty> or PATH=:<empty> */
	if (!pth || !pth[0] || LONE_CHAR(pth, ':'))
		return 1;

	tmp = pth;
	npth = 0;

	while (1) {
		npth++;                 /* count words is + 1 count ':' */
		tmp = strchr(tmp, ':');
		if (!tmp)
			break;
		if (*++tmp == '\0')
			break;  /* :<empty> */
	}

	*p = xmalloc(npth * sizeof(char *));

	tmp = pth;
	(*p)[0] = xstrdup(tmp);
	npth = 1;                       /* count words is + 1 count ':' */

	while (1) {
		tmp = strchr(tmp, ':');
		if (!tmp)
			break;
		(*p)[0][(tmp - pth)] = 0;       /* ':' -> '\0' */
		if (*++tmp == 0)
			break;                  /* :<empty> */
		(*p)[npth++] = &(*p)[0][(tmp - pth)];   /* p[next]=p[0][&'\0'+1] */
	}

	return npth;
}

static void exe_n_cwd_tab_completion(char *command, int type)
{
	DIR *dir;
	struct dirent *next;
	char dirbuf[BUFSIZ];
	struct stat st;
	char *path1[1];
	char **paths = path1;
	int npaths;
	int i;
	char *found;
	char *pfind = strrchr(command, '/');

	npaths = 1;
	path1[0] = ".";

	if (pfind == NULL) {
		/* no dir, if flags==EXE_ONLY - get paths, else "." */
		npaths = path_parse(&paths, type);
		pfind = command;
	} else {
		/* dirbuf = ".../.../.../" */
		safe_strncpy(dirbuf, command, (pfind - command) + 2);
#if ENABLE_FEATURE_COMMAND_USERNAME_COMPLETION
		if (dirbuf[0] == '~')   /* ~/... or ~user/... */
			username_tab_completion(dirbuf, dirbuf);
#endif
		paths[0] = dirbuf;
		/* point to 'l' in "..../last_component" */
		pfind++;
	}

	for (i = 0; i < npaths; i++) {

		dir = opendir(paths[i]);
		if (!dir)                       /* Don't print an error */
			continue;

		while ((next = readdir(dir)) != NULL) {
			int len1;
			char *str_found = next->d_name;

			/* matched? */
			if (strncmp(str_found, pfind, strlen(pfind)))
				continue;
			/* not see .name without .match */
			if (*str_found == '.' && *pfind == 0) {
				if (NOT_LONE_CHAR(paths[i], '/') || str_found[1])
					continue;
				str_found = ""; /* only "/" */
			}
			found = concat_path_file(paths[i], str_found);
			/* hmm, remover in progress? */
			if (stat(found, &st) < 0)
				goto cont;
			/* find with dirs? */
			if (paths[i] != dirbuf)
				strcpy(found, next->d_name);    /* only name */

			len1 = strlen(found);
			found = xrealloc(found, len1 + 2);
			found[len1] = '\0';
			found[len1+1] = '\0';

			if (S_ISDIR(st.st_mode)) {
				/* name is directory      */
				if (found[len1-1] != '/') {
					found[len1] = '/';
				}
			} else {
				/* not put found file if search only dirs for cd */
				if (type == FIND_DIR_ONLY)
					goto cont;
			}
			/* Add it to the list */
			add_match(found);
			continue;
 cont:
			free(found);
		}
		closedir(dir);
	}
	if (paths != path1) {
		free(paths[0]);                 /* allocated memory only in first member */
		free(paths);
	}
}


#define QUOT (UCHAR_MAX+1)

#define collapse_pos(is, in) { \
	memmove(int_buf+(is), int_buf+(in), (BUFSIZ+1-(is)-(in))*sizeof(int)); \
	memmove(pos_buf+(is), pos_buf+(in), (BUFSIZ+1-(is)-(in))*sizeof(int)); }

static int find_match(char *matchBuf, int *len_with_quotes)
{
	int i, j;
	int command_mode;
	int c, c2;
	int int_buf[BUFSIZ + 1];
	int pos_buf[BUFSIZ + 1];

	/* set to integer dimension characters and own positions */
	for (i = 0;; i++) {
		int_buf[i] = (unsigned char)matchBuf[i];
		if (int_buf[i] == 0) {
			pos_buf[i] = -1;        /* indicator end line */
			break;
		} else
			pos_buf[i] = i;
	}

	/* mask \+symbol and convert '\t' to ' ' */
	for (i = j = 0; matchBuf[i]; i++, j++)
		if (matchBuf[i] == '\\') {
			collapse_pos(j, j + 1);
			int_buf[j] |= QUOT;
			i++;
#if ENABLE_FEATURE_NONPRINTABLE_INVERSE_PUT
			if (matchBuf[i] == '\t')        /* algorithm equivalent */
				int_buf[j] = ' ' | QUOT;
#endif
		}
#if ENABLE_FEATURE_NONPRINTABLE_INVERSE_PUT
		else if (matchBuf[i] == '\t')
			int_buf[j] = ' ';
#endif

	/* mask "symbols" or 'symbols' */
	c2 = 0;
	for (i = 0; int_buf[i]; i++) {
		c = int_buf[i];
		if (c == '\'' || c == '"') {
			if (c2 == 0)
				c2 = c;
			else {
				if (c == c2)
					c2 = 0;
				else
					int_buf[i] |= QUOT;
			}
		} else if (c2 != 0 && c != '$')
			int_buf[i] |= QUOT;
	}

	/* skip commands with arguments if line has commands delimiters */
	/* ';' ';;' '&' '|' '&&' '||' but `>&' `<&' `>|' */
	for (i = 0; int_buf[i]; i++) {
		c = int_buf[i];
		c2 = int_buf[i + 1];
		j = i ? int_buf[i - 1] : -1;
		command_mode = 0;
		if (c == ';' || c == '&' || c == '|') {
			command_mode = 1 + (c == c2);
			if (c == '&') {
				if (j == '>' || j == '<')
					command_mode = 0;
			} else if (c == '|' && j == '>')
				command_mode = 0;
		}
		if (command_mode) {
			collapse_pos(0, i + command_mode);
			i = -1;                         /* hack incremet */
		}
	}
	/* collapse `command...` */
	for (i = 0; int_buf[i]; i++)
		if (int_buf[i] == '`') {
			for (j = i + 1; int_buf[j]; j++)
				if (int_buf[j] == '`') {
					collapse_pos(i, j + 1);
					j = 0;
					break;
				}
			if (j) {
				/* not found close ` - command mode, collapse all previous */
				collapse_pos(0, i + 1);
				break;
			} else
				i--;                    /* hack incremet */
		}

	/* collapse (command...(command...)...) or {command...{command...}...} */
	c = 0;                                          /* "recursive" level */
	c2 = 0;
	for (i = 0; int_buf[i]; i++)
		if (int_buf[i] == '(' || int_buf[i] == '{') {
			if (int_buf[i] == '(')
				c++;
			else
				c2++;
			collapse_pos(0, i + 1);
			i = -1;                         /* hack incremet */
		}
	for (i = 0; pos_buf[i] >= 0 && (c > 0 || c2 > 0); i++)
		if ((int_buf[i] == ')' && c > 0) || (int_buf[i] == '}' && c2 > 0)) {
			if (int_buf[i] == ')')
				c--;
			else
				c2--;
			collapse_pos(0, i + 1);
			i = -1;                         /* hack incremet */
		}

	/* skip first not quote space */
	for (i = 0; int_buf[i]; i++)
		if (int_buf[i] != ' ')
			break;
	if (i)
		collapse_pos(0, i);

	/* set find mode for completion */
	command_mode = FIND_EXE_ONLY;
	for (i = 0; int_buf[i]; i++)
		if (int_buf[i] == ' ' || int_buf[i] == '<' || int_buf[i] == '>') {
			if (int_buf[i] == ' ' && command_mode == FIND_EXE_ONLY
			 && matchBuf[pos_buf[0]]=='c'
			 && matchBuf[pos_buf[1]]=='d'
			) {
				command_mode = FIND_DIR_ONLY;
			} else {
				command_mode = FIND_FILE_ONLY;
				break;
			}
		}
	for (i = 0; int_buf[i]; i++)
		/* "strlen" */;
	/* find last word */
	for (--i; i >= 0; i--) {
		c = int_buf[i];
		if (c == ' ' || c == '<' || c == '>' || c == '|' || c == '&') {
			collapse_pos(0, i + 1);
			break;
		}
	}
	/* skip first not quoted '\'' or '"' */
	for (i = 0; int_buf[i] == '\'' || int_buf[i] == '"'; i++)
		/*skip*/;
	/* collapse quote or unquote // or /~ */
	while ((int_buf[i] & ~QUOT) == '/'
	 && ((int_buf[i+1] & ~QUOT) == '/' || (int_buf[i+1] & ~QUOT) == '~')
	) {
		i++;
	}

	/* set only match and destroy quotes */
	j = 0;
	for (c = 0; pos_buf[i] >= 0; i++) {
		matchBuf[c++] = matchBuf[pos_buf[i]];
		j = pos_buf[i] + 1;
	}
	matchBuf[c] = 0;
	/* old lenght matchBuf with quotes symbols */
	*len_with_quotes = j ? j - pos_buf[0] : 0;

	return command_mode;
}

/*
   display by column original ideas from ls applet,
   very optimize by my :)
*/
static void showfiles(void)
{
	int ncols, row;
	int column_width = 0;
	int nfiles = num_matches;
	int nrows = nfiles;
	int l;

	/* find the longest file name-  use that as the column width */
	for (row = 0; row < nrows; row++) {
		l = strlen(matches[row]);
		if (column_width < l)
			column_width = l;
	}
	column_width += 2;              /* min space for columns */
	ncols = cmdedit_termw / column_width;

	if (ncols > 1) {
		nrows /= ncols;
		if (nfiles % ncols)
			nrows++;        /* round up fractionals */
	} else {
		ncols = 1;
	}
	for (row = 0; row < nrows; row++) {
		int n = row;
		int nc;

		for (nc = 1; nc < ncols && n+nrows < nfiles; n += nrows, nc++) {
			printf("%s%-*s", matches[n],
				(int)(column_width - strlen(matches[n])), "");
		}
		printf("%s\n", matches[n]);
	}
}

static int match_compare(const void *a, const void *b)
{
	return strcmp(*(char**)a, *(char**)b);
}

static char *add_quote_for_spec_chars(char *found)
{
	int l = 0;
	char *s = xmalloc((strlen(found) + 1) * 2);

	while (*found) {
		if (strchr(" `\"#$%^&*()=+{}[]:;\'|\\<>", *found))
			s[l++] = '\\';
		s[l++] = *found++;
	}
	s[l] = 0;
	return s;
}

static void input_tab(int *lastWasTab)
{
	/* Do TAB completion */
	if (lastWasTab == 0) {          /* free all memory */
		if (matches) {
			while (num_matches > 0)
				free(matches[--num_matches]);
			free(matches);
			matches = (char **) NULL;
		}
		return;
	}
	if (!*lastWasTab) {
		char *tmp, *tmp1;
		int len_found;
		char matchBuf[BUFSIZ];
		int find_type;
		int recalc_pos;

		*lastWasTab = TRUE;             /* flop trigger */

		/* Make a local copy of the string -- up
		 * to the position of the cursor */
		tmp = strncpy(matchBuf, command_ps, cursor);
		tmp[cursor] = 0;

		find_type = find_match(matchBuf, &recalc_pos);

		/* Free up any memory already allocated */
		input_tab(0);

#if ENABLE_FEATURE_COMMAND_USERNAME_COMPLETION
		/* If the word starts with `~' and there is no slash in the word,
		 * then try completing this word as a username. */

		if (matchBuf[0] == '~' && strchr(matchBuf, '/') == 0)
			username_tab_completion(matchBuf, NULL);
		if (!matches)
#endif
		/* Try to match any executable in our path and everything
		 * in the current working directory that matches.  */
			exe_n_cwd_tab_completion(matchBuf, find_type);
		/* Sort, then remove any duplicates found */
		if (matches) {
			int i, n = 0;
			qsort(matches, num_matches, sizeof(char*), match_compare);
			for (i = 0; i < num_matches - 1; ++i) {
				if (matches[i] && matches[i+1]) {
					if (strcmp(matches[i], matches[i+1]) == 0) {
						free(matches[i]);
						matches[i] = 0;
					} else {
						matches[n++] = matches[i];
					}
				}
			}
			matches[n++] = matches[num_matches-1];
			num_matches = n;
		}
		/* Did we find exactly one match? */
		if (!matches || num_matches > 1) {
			beep();
			if (!matches)
				return;         /* not found */
			/* find minimal match */
			tmp1 = xstrdup(matches[0]);
			for (tmp = tmp1; *tmp; tmp++)
				for (len_found = 1; len_found < num_matches; len_found++)
					if (matches[len_found][(tmp - tmp1)] != *tmp) {
						*tmp = 0;
						break;
					}
			if (*tmp1 == 0) {        /* have unique */
				free(tmp1);
				return;
			}
			tmp = add_quote_for_spec_chars(tmp1);
			free(tmp1);
		} else {                        /* one match */
			tmp = add_quote_for_spec_chars(matches[0]);
			/* for next completion current found */
			*lastWasTab = FALSE;

			len_found = strlen(tmp);
			if (tmp[len_found-1] != '/') {
				tmp[len_found] = ' ';
				tmp[len_found+1] = '\0';
			}
		}
		len_found = strlen(tmp);
		/* have space to placed match? */
		if ((len_found - strlen(matchBuf) + len) < BUFSIZ) {

			/* before word for match   */
			command_ps[cursor - recalc_pos] = 0;
			/* save   tail line        */
			strcpy(matchBuf, command_ps + cursor);
			/* add    match            */
			strcat(command_ps, tmp);
			/* add    tail             */
			strcat(command_ps, matchBuf);
			/* back to begin word for match    */
			input_backward(recalc_pos);
			/* new pos                         */
			recalc_pos = cursor + len_found;
			/* new len                         */
			len = strlen(command_ps);
			/* write out the matched command   */
			redraw(cmdedit_y, len - recalc_pos);
		}
		free(tmp);
	} else {
		/* Ok -- the last char was a TAB.  Since they
		 * just hit TAB again, print a list of all the
		 * available choices... */
		if (matches && num_matches > 0) {
			int sav_cursor = cursor;        /* change goto_new_line() */

			/* Go to the next line */
			goto_new_line();
			showfiles();
			redraw(0, len - sav_cursor);
		}
	}
}
#endif  /* FEATURE_COMMAND_TAB_COMPLETION */


#if MAX_HISTORY > 0

static char *history[MAX_HISTORY+1]; /* history + current */
/* saved history lines */
static int n_history;
/* current pointer to history line */
static int cur_history;

static void get_previous_history(void)
{
	if (command_ps[0] != 0 || history[cur_history] == 0) {
		free(history[cur_history]);
		history[cur_history] = xstrdup(command_ps);
	}
	cur_history--;
}

static int get_next_history(void)
{
	int ch = cur_history;

	if (ch < n_history) {
		get_previous_history(); /* save the current history line */
		cur_history = ch + 1;
		return cur_history;
	} else {
		beep();
		return 0;
	}
}

#if ENABLE_FEATURE_COMMAND_SAVEHISTORY
void load_history(const char *fromfile)
{
	FILE *fp;
	int hi;

	/* cleanup old */

	for (hi = n_history; hi > 0;) {
		hi--;
		free(history[hi]);
	}

	fp = fopen(fromfile, "r");
	if (fp) {
		for (hi = 0; hi < MAX_HISTORY;) {
			char * hl = xmalloc_getline(fp);
			int l;

			if (!hl)
				break;
			l = strlen(hl);
			if (l >= BUFSIZ)
				hl[BUFSIZ-1] = 0;
			if (l == 0 || hl[0] == ' ') {
				free(hl);
				continue;
			}
			history[hi++] = hl;
		}
		fclose(fp);
	}
	cur_history = n_history = hi;
}

void save_history (const char *tofile)
{
	FILE *fp = fopen(tofile, "w");

	if (fp) {
		int i;

		for (i = 0; i < n_history; i++) {
			fprintf(fp, "%s\n", history[i]);
		}
		fclose(fp);
	}
}
#endif /* FEATURE_COMMAND_SAVEHISTORY */

#endif /* MAX_HISTORY > 0 */


/*
 * This function is used to grab a character buffer
 * from the input file descriptor and allows you to
 * a string with full command editing (sort of like
 * a mini readline).
 *
 * The following standard commands are not implemented:
 * ESC-b -- Move back one word
 * ESC-f -- Move forward one word
 * ESC-d -- Delete back one word
 * ESC-h -- Delete forward one word
 * CTL-t -- Transpose two characters
 *
 * Minimalist vi-style command line editing available if configured.
 * vi mode implemented 2005 by Paul Fox <pgf@foxharp.boston.ma.us>
 */

#if ENABLE_FEATURE_COMMAND_EDITING_VI
static int vi_mode;

void setvimode(int viflag)
{
	vi_mode = viflag;
}

static void
vi_Word_motion(char *command, int eat)
{
	while (cursor < len && !isspace(command[cursor]))
		input_forward();
	if (eat) while (cursor < len && isspace(command[cursor]))
		input_forward();
}

static void
vi_word_motion(char *command, int eat)
{
	if (isalnum(command[cursor]) || command[cursor] == '_') {
		while (cursor < len
		 && (isalnum(command[cursor+1]) || command[cursor+1] == '_'))
			input_forward();
	} else if (ispunct(command[cursor])) {
		while (cursor < len && ispunct(command[cursor+1]))
			input_forward();
	}

	if (cursor < len)
		input_forward();

	if (eat && cursor < len && isspace(command[cursor]))
		while (cursor < len && isspace(command[cursor]))
			input_forward();
}

static void
vi_End_motion(char *command)
{
	input_forward();
	while (cursor < len && isspace(command[cursor]))
		input_forward();
	while (cursor < len-1 && !isspace(command[cursor+1]))
		input_forward();
}

static void
vi_end_motion(char *command)
{
	if (cursor >= len-1)
		return;
	input_forward();
	while (cursor < len-1 && isspace(command[cursor]))
		input_forward();
	if (cursor >= len-1)
		return;
	if (isalnum(command[cursor]) || command[cursor] == '_') {
		while (cursor < len-1
		 && (isalnum(command[cursor+1]) || command[cursor+1] == '_')
		) {
			input_forward();
		}
	} else if (ispunct(command[cursor])) {
		while (cursor < len-1 && ispunct(command[cursor+1]))
			input_forward();
	}
}

static void
vi_Back_motion(char *command)
{
	while (cursor > 0 && isspace(command[cursor-1]))
		input_backward(1);
	while (cursor > 0 && !isspace(command[cursor-1]))
		input_backward(1);
}

static void
vi_back_motion(char *command)
{
	if (cursor <= 0)
		return;
	input_backward(1);
	while (cursor > 0 && isspace(command[cursor]))
		input_backward(1);
	if (cursor <= 0)
		return;
	if (isalnum(command[cursor]) || command[cursor] == '_') {
		while (cursor > 0
		 && (isalnum(command[cursor-1]) || command[cursor-1] == '_')
		) {
			input_backward(1);
		}
	} else if (ispunct(command[cursor])) {
		while (cursor > 0 && ispunct(command[cursor-1]))
			input_backward(1);
	}
}
#else
enum { vi_mode = 0 };
#endif


/*
 * cmdedit_read_input and its helpers
 */

#define setTermSettings(fd, argp) tcsetattr(fd, TCSANOW, argp)
#define getTermSettings(fd, argp) tcgetattr(fd, argp);

static sighandler_t previous_SIGWINCH_handler;

static void cmdedit_reset_term(void)
{
	if (handlers_sets & SET_RESET_TERM) {
		setTermSettings(STDIN_FILENO, (void *) &initial_settings);
		handlers_sets &= ~SET_RESET_TERM;
	}
	if (handlers_sets & SET_WCHG_HANDLERS) {
		/* restore SIGWINCH handler */
		signal(SIGWINCH, previous_SIGWINCH_handler);
		handlers_sets &= ~SET_WCHG_HANDLERS;
	}
	fflush(stdout);
}

static void cmdedit_setwidth(unsigned w, int redraw_flg)
{
	cmdedit_termw = w;
	if (redraw_flg) {
		/* new y for current cursor */
		int new_y = (cursor + cmdedit_prmt_len) / w;
		/* redraw */
		redraw((new_y >= cmdedit_y ? new_y : cmdedit_y), len - cursor);
		fflush(stdout);
	}
}

static void win_changed(int nsig)
{
	int width;
	get_terminal_width_height(0, &width, NULL);
	cmdedit_setwidth(width, nsig /* - just a yes/no flag */);
	if (nsig == SIGWINCH)
		signal(SIGWINCH, win_changed); /* rearm ourself */
}

static void cmdedit_init(void)
{
	cmdedit_prmt_len = 0;
	if (!(handlers_sets & SET_WCHG_HANDLERS)) {
		previous_SIGWINCH_handler = signal(SIGWINCH, win_changed);
		win_changed(0); /* do initial resizing */
		handlers_sets |= SET_WCHG_HANDLERS;
	}

	if (!(handlers_sets & SET_ATEXIT)) {
#if ENABLE_FEATURE_GETUSERNAME_AND_HOMEDIR
		struct passwd *entry;

		my_euid = geteuid();
		entry = getpwuid(my_euid);
		if (entry) {
			user_buf = xstrdup(entry->pw_name);
			home_pwd_buf = xstrdup(entry->pw_dir);
		}
#endif

#if ENABLE_FEATURE_COMMAND_TAB_COMPLETION

#if !ENABLE_FEATURE_GETUSERNAME_AND_HOMEDIR
		my_euid = geteuid();
#endif
		my_uid = getuid();
		my_gid = getgid();
#endif  /* FEATURE_COMMAND_TAB_COMPLETION */
		handlers_sets |= SET_ATEXIT;
// Crap. We should be able to do it without atexit.
		atexit(cmdedit_reset_term);     /* be sure to do this only once */
	}
}

#if !ENABLE_FEATURE_SH_FANCY_PROMPT
static void parse_prompt(const char *prmt_ptr)
{
	cmdedit_prompt = prmt_ptr;
	cmdedit_prmt_len = strlen(prmt_ptr);
	put_prompt();
}
#else
static void parse_prompt(const char *prmt_ptr)
{
	int prmt_len = 0;
	size_t cur_prmt_len = 0;
	char flg_not_length = '[';
	char *prmt_mem_ptr = xzalloc(1);
	char *pwd_buf = xgetcwd(0);
	char buf2[PATH_MAX + 1];
	char buf[2];
	char c;
	char *pbuf;

	if (!pwd_buf) {
		pwd_buf = (char *)bb_msg_unknown;
	}

	while (*prmt_ptr) {
		pbuf = buf;
		pbuf[1] = 0;
		c = *prmt_ptr++;
		if (c == '\\') {
			const char *cp = prmt_ptr;
			int l;

			c = bb_process_escape_sequence(&prmt_ptr);
			if (prmt_ptr == cp) {
				if (*cp == 0)
					break;
				c = *prmt_ptr++;
				switch (c) {
#if ENABLE_FEATURE_GETUSERNAME_AND_HOMEDIR
				case 'u':
					pbuf = user_buf;
					break;
#endif
				case 'h':
					pbuf = hostname_buf;
					if (!pbuf) {
						pbuf = xzalloc(256);
						if (gethostname(pbuf, 255) < 0) {
							strcpy(pbuf, "?");
						} else {
							char *s = strchr(pbuf, '.');
							if (s)
								*s = '\0';
						}
						hostname_buf = pbuf;
					}
					break;
				case '$':
					c = (my_euid == 0 ? '#' : '$');
					break;
#if ENABLE_FEATURE_GETUSERNAME_AND_HOMEDIR
				case 'w':
					pbuf = pwd_buf;
					l = strlen(home_pwd_buf);
					if (home_pwd_buf[0] != 0
					 && strncmp(home_pwd_buf, pbuf, l) == 0
					 && (pbuf[l]=='/' || pbuf[l]=='\0')
					 && strlen(pwd_buf+l)<PATH_MAX
					) {
						pbuf = buf2;
						*pbuf = '~';
						strcpy(pbuf+1, pwd_buf+l);
					}
					break;
#endif
				case 'W':
					pbuf = pwd_buf;
					cp = strrchr(pbuf,'/');
					if (cp != NULL && cp != pbuf)
						pbuf += (cp-pbuf) + 1;
					break;
				case '!':
					snprintf(pbuf = buf2, sizeof(buf2), "%d", num_ok_lines);
					break;
				case 'e': case 'E':     /* \e \E = \033 */
					c = '\033';
					break;
				case 'x': case 'X':
					for (l = 0; l < 3;) {
						int h;
						buf2[l++] = *prmt_ptr;
						buf2[l] = 0;
						h = strtol(buf2, &pbuf, 16);
						if (h > UCHAR_MAX || (pbuf - buf2) < l) {
							l--;
							break;
						}
						prmt_ptr++;
					}
					buf2[l] = 0;
					c = (char)strtol(buf2, NULL, 16);
					if (c == 0)
						c = '?';
					pbuf = buf;
					break;
				case '[': case ']':
					if (c == flg_not_length) {
						flg_not_length = flg_not_length == '[' ? ']' : '[';
						continue;
					}
					break;
				}
			}
		}
		if (pbuf == buf)
			*pbuf = c;
		cur_prmt_len = strlen(pbuf);
		prmt_len += cur_prmt_len;
		if (flg_not_length != ']')
			cmdedit_prmt_len += cur_prmt_len;
		prmt_mem_ptr = strcat(xrealloc(prmt_mem_ptr, prmt_len+1), pbuf);
	}
	if (pwd_buf!=(char *)bb_msg_unknown)
		free(pwd_buf);
	cmdedit_prompt = prmt_mem_ptr;
	put_prompt();
}
#endif

/*
 * the emacs and vi modes share much of the code in the big
 * command loop.  commands entered when in vi's command mode (aka
 * "escape mode") get an extra bit added to distinguish them --
 * this keeps them from being self-inserted.  this clutters the
 * big switch a bit, but keeps all the code in one place.
 */

#define vbit 0x100

/* leave out the "vi-mode"-only case labels if vi editing isn't
 * configured. */
#define vi_case(caselabel) USE_FEATURE_COMMAND_EDITING(case caselabel)

/* convert uppercase ascii to equivalent control char, for readability */
#undef CTRL
#define CTRL(a) ((a) & ~0x40)


int cmdedit_read_input(char *prompt, char command[BUFSIZ])
{
	int lastWasTab = FALSE;
	unsigned int ic;
	unsigned char c;
	smallint break_out = 0;
#if ENABLE_FEATURE_COMMAND_EDITING_VI
	smallint vi_cmdmode = 0;
	smalluint prevc;
#endif
	/* prepare before init handlers */
	cmdedit_y = 0;  /* quasireal y, not true if line > xt*yt */
	len = 0;
	command_ps = command;
	command[0] = '\0';

	getTermSettings(0, (void *) &initial_settings);
	memcpy(&new_settings, &initial_settings, sizeof(struct termios));
	new_settings.c_lflag &= ~ICANON;        /* unbuffered input */
	/* Turn off echoing and CTRL-C, so we can trap it */
	new_settings.c_lflag &= ~(ECHO | ECHONL | ISIG);
	/* Hmm, in linux c_cc[] not parsed if set ~ICANON */
	new_settings.c_cc[VMIN] = 1;
	new_settings.c_cc[VTIME] = 0;
	/* Turn off CTRL-C, so we can trap it */
#ifndef _POSIX_VDISABLE
#define _POSIX_VDISABLE '\0'
#endif
	new_settings.c_cc[VINTR] = _POSIX_VDISABLE;
	setTermSettings(0, (void *) &new_settings);
	handlers_sets |= SET_RESET_TERM;

	/* Now initialize things */
	cmdedit_init();
	/* Print out the command prompt */
	parse_prompt(prompt);

	while (1) {
		fflush(stdout);                 /* buffered out to fast */

		if (safe_read(0, &c, 1) < 1)
			/* if we can't read input then exit */
			goto prepare_to_die;

		ic = c;

#if ENABLE_FEATURE_COMMAND_EDITING_VI
		newdelflag = 1;
		if (vi_cmdmode)
			ic |= vbit;
#endif
		switch (ic) {
		case '\n':
		case '\r':
		vi_case('\n'|vbit:)
		vi_case('\r'|vbit:)
			/* Enter */
			goto_new_line();
			break_out = 1;
			break;
		case CTRL('A'):
		vi_case('0'|vbit:)
			/* Control-a -- Beginning of line */
			input_backward(cursor);
			break;
		case CTRL('B'):
		vi_case('h'|vbit:)
		vi_case('\b'|vbit:)
		vi_case('\x7f'|vbit:) /* DEL */
			/* Control-b -- Move back one character */
			input_backward(1);
			break;
		case CTRL('C'):
		vi_case(CTRL('C')|vbit:)
			/* Control-c -- stop gathering input */
			goto_new_line();
#if !ENABLE_ASH
			command[0] = '\0';
			len = 0;
			lastWasTab = FALSE;
			put_prompt();
#else
			len = 0;
			break_out = -1; /* to control traps */
#endif
			break;
		case CTRL('D'):
			/* Control-d -- Delete one character, or exit
			 * if the len=0 and no chars to delete */
			if (len == 0) {
				errno = 0;
 prepare_to_die:
// So, our API depends on whether we have ash compiled in or not? Crap...
#if !ENABLE_ASH
				printf("exit");
				goto_new_line();
				/* cmdedit_reset_term() called in atexit */
// FIXME. this is definitely not good
				exit(EXIT_SUCCESS);
#else
				/* to control stopped jobs */
				break_out = len = -1;
				break;
#endif
			} else {
				input_delete(0);
			}
			break;
		case CTRL('E'):
		vi_case('$'|vbit:)
			/* Control-e -- End of line */
			input_end();
			break;
		case CTRL('F'):
		vi_case('l'|vbit:)
		vi_case(' '|vbit:)
			/* Control-f -- Move forward one character */
			input_forward();
			break;
		case '\b':
		case '\x7f': /* DEL */
			/* Control-h and DEL */
			input_backspace();
			break;
		case '\t':
#if ENABLE_FEATURE_COMMAND_TAB_COMPLETION
			input_tab(&lastWasTab);
#endif
			break;
		case CTRL('K'):
			/* Control-k -- clear to end of line */
			command[cursor] = 0;
			len = cursor;
			printf("\033[J");
			break;
		case CTRL('L'):
		vi_case(CTRL('L')|vbit:)
			/* Control-l -- clear screen */
			printf("\033[H");
			redraw(0, len - cursor);
			break;
#if MAX_HISTORY > 0
		case CTRL('N'):
		vi_case(CTRL('N')|vbit:)
		vi_case('j'|vbit:)
			/* Control-n -- Get next command in history */
			if (get_next_history())
				goto rewrite_line;
			break;
		case CTRL('P'):
		vi_case(CTRL('P')|vbit:)
		vi_case('k'|vbit:)
			/* Control-p -- Get previous command from history */
			if (cur_history > 0) {
				get_previous_history();
				goto rewrite_line;
			} else {
				beep();
			}
			break;
#endif
		case CTRL('U'):
		vi_case(CTRL('U')|vbit:)
			/* Control-U -- Clear line before cursor */
			if (cursor) {
				strcpy(command, command + cursor);
				redraw(cmdedit_y, len -= cursor);
			}
			break;
		case CTRL('W'):
		vi_case(CTRL('W')|vbit:)
			/* Control-W -- Remove the last word */
			while (cursor > 0 && isspace(command[cursor-1]))
				input_backspace();
			while (cursor > 0 &&!isspace(command[cursor-1]))
				input_backspace();
			break;
#if ENABLE_FEATURE_COMMAND_EDITING_VI
		case 'i'|vbit:
			vi_cmdmode = 0;
			break;
		case 'I'|vbit:
			input_backward(cursor);
			vi_cmdmode = 0;
			break;
		case 'a'|vbit:
			input_forward();
			vi_cmdmode = 0;
			break;
		case 'A'|vbit:
			input_end();
			vi_cmdmode = 0;
			break;
		case 'x'|vbit:
			input_delete(1);
			break;
		case 'X'|vbit:
			if (cursor > 0) {
				input_backward(1);
				input_delete(1);
			}
			break;
		case 'W'|vbit:
			vi_Word_motion(command, 1);
			break;
		case 'w'|vbit:
			vi_word_motion(command, 1);
			break;
		case 'E'|vbit:
			vi_End_motion(command);
			break;
		case 'e'|vbit:
			vi_end_motion(command);
			break;
		case 'B'|vbit:
			vi_Back_motion(command);
			break;
		case 'b'|vbit:
			vi_back_motion(command);
			break;
		case 'C'|vbit:
			vi_cmdmode = 0;
			/* fall through */
		case 'D'|vbit:
			goto clear_to_eol;

		case 'c'|vbit:
			vi_cmdmode = 0;
			/* fall through */
		case 'd'|vbit: {
			int nc, sc;
			sc = cursor;
			prevc = ic;
			if (safe_read(0, &c, 1) < 1)
				goto prepare_to_die;
			if (c == (prevc & 0xff)) {
				/* "cc", "dd" */
				input_backward(cursor);
				goto clear_to_eol;
				break;
			}
			switch (c) {
			case 'w':
			case 'W':
			case 'e':
			case 'E':
				switch (c) {
				case 'w':   /* "dw", "cw" */
					vi_word_motion(command, vi_cmdmode);
					break;
				case 'W':   /* 'dW', 'cW' */
					vi_Word_motion(command, vi_cmdmode);
					break;
				case 'e':   /* 'de', 'ce' */
					vi_end_motion(command);
					input_forward();
					break;
				case 'E':   /* 'dE', 'cE' */
					vi_End_motion(command);
					input_forward();
					break;
				}
				nc = cursor;
				input_backward(cursor - sc);
				while (nc-- > cursor)
					input_delete(1);
				break;
			case 'b':  /* "db", "cb" */
			case 'B':  /* implemented as B */
				if (c == 'b')
					vi_back_motion(command);
				else
					vi_Back_motion(command);
				while (sc-- > cursor)
					input_delete(1);
				break;
			case ' ':  /* "d ", "c " */
				input_delete(1);
				break;
			case '$':  /* "d$", "c$" */
			clear_to_eol:
				while (cursor < len)
					input_delete(1);
				break;
			}
			break;
		}
		case 'p'|vbit:
			input_forward();
			/* fallthrough */
		case 'P'|vbit:
			put();
			break;
		case 'r'|vbit:
			if (safe_read(0, &c, 1) < 1)
				goto prepare_to_die;
			if (c == 0)
				beep();
			else {
				*(command + cursor) = c;
				putchar(c);
				putchar('\b');
			}
			break;
#endif /* FEATURE_COMMAND_EDITING_VI */

		case '\x1b': /* ESC */

#if ENABLE_FEATURE_COMMAND_EDITING_VI
			if (vi_mode) {
				/* ESC: insert mode --> command mode */
				vi_cmdmode = 1;
				input_backward(1);
				break;
			}
#endif
			/* escape sequence follows */
			if (safe_read(0, &c, 1) < 1)
				goto prepare_to_die;
			/* different vt100 emulations */
			if (c == '[' || c == 'O') {
		vi_case('['|vbit:)
		vi_case('O'|vbit:)
				if (safe_read(0, &c, 1) < 1)
					goto prepare_to_die;
			}
			if (c >= '1' && c <= '9') {
				unsigned char dummy;

				if (safe_read(0, &dummy, 1) < 1)
					goto prepare_to_die;
				if (dummy != '~')
					c = '\0';
			}
			switch (c) {
#if ENABLE_FEATURE_COMMAND_TAB_COMPLETION
			case '\t':                      /* Alt-Tab */
				input_tab(&lastWasTab);
				break;
#endif
#if MAX_HISTORY > 0
			case 'A':
				/* Up Arrow -- Get previous command from history */
				if (cur_history > 0) {
					get_previous_history();
					goto rewrite_line;
				}
				beep();
				break;
			case 'B':
				/* Down Arrow -- Get next command in history */
				if (!get_next_history())
					break;
 rewrite_line:
				/* Rewrite the line with the selected history item */
				/* change command */
				len = strlen(strcpy(command, history[cur_history]));
				/* redraw and go to eol (bol, in vi */
				redraw(cmdedit_y, vi_mode ? 9999 : 0);
				break;
#endif
			case 'C':
				/* Right Arrow -- Move forward one character */
				input_forward();
				break;
			case 'D':
				/* Left Arrow -- Move back one character */
				input_backward(1);
				break;
			case '3':
				/* Delete */
				input_delete(0);
				break;
			case '1':
			case 'H':
				/* <Home> */
				input_backward(cursor);
				break;
			case '4':
			case 'F':
				/* <End> */
				input_end();
				break;
			default:
				c = 0;
				beep();
			}
			break;

		default:        /* If it's regular input, do the normal thing */
#if ENABLE_FEATURE_NONPRINTABLE_INVERSE_PUT
			/* Control-V -- Add non-printable symbol */
			if (c == CTRL('V')) {
				if (safe_read(0, &c, 1) < 1)
					goto prepare_to_die;
				if (c == 0) {
					beep();
					break;
				}
			} else
#endif
			{
#if ENABLE_FEATURE_COMMAND_EDITING_VI
				if (vi_cmdmode)  /* don't self-insert */
					break;
#endif
				if (!Isprint(c)) /* Skip non-printable characters */
					break;
			}

			if (len >= (BUFSIZ - 2))        /* Need to leave space for enter */
				break;

			len++;

			if (cursor == (len - 1)) {      /* Append if at the end of the line */
				*(command + cursor) = c;
				*(command + cursor + 1) = 0;
				cmdedit_set_out_char(' ');
			} else {                        /* Insert otherwise */
				int sc = cursor;

				memmove(command + sc + 1, command + sc, len - sc);
				*(command + sc) = c;
				sc++;
				/* rewrite from cursor */
				input_end();
				/* to prev x pos + 1 */
				input_backward(cursor - sc);
			}

			break;
		}
		if (break_out)                  /* Enter is the command terminator, no more input. */
			break;

		if (c != '\t')
			lastWasTab = FALSE;
	}

	setTermSettings(0, (void *) &initial_settings);
	handlers_sets &= ~SET_RESET_TERM;

#if MAX_HISTORY > 0
	/* Handle command history log */
	/* cleanup may be saved current command line */
	if (len > 0) {                                      /* no put empty line */
		int i = n_history;

		free(history[MAX_HISTORY]);
		history[MAX_HISTORY] = 0;
		/* After max history, remove the oldest command */
		if (i >= MAX_HISTORY) {
			free(history[0]);
			for (i = 0; i < MAX_HISTORY-1; i++)
				history[i] = history[i+1];
		}
		history[i++] = xstrdup(command);
		cur_history = i;
		n_history = i;
		USE_FEATURE_SH_FANCY_PROMPT(num_ok_lines++;)
	}
#else /* MAX_HISTORY == 0 */
	/* dont put empty line */
	USE_FEATURE_SH_FANCY_PROMPT(if (len > 0) num_ok_lines++;)
#endif /* MAX_HISTORY */

	if (break_out > 0) {
		command[len++] = '\n';
		command[len] = '\0';
	}

#if ENABLE_FEATURE_CLEAN_UP && ENABLE_FEATURE_COMMAND_TAB_COMPLETION
	input_tab(0);
#endif

#if ENABLE_FEATURE_SH_FANCY_PROMPT
	free(cmdedit_prompt);
#endif
	cmdedit_reset_term();
	return len;
}

#endif  /* FEATURE_COMMAND_EDITING */


/*
 * Testing
 */

#ifdef TEST

#include <locale.h>

const char *applet_name = "debug stuff usage";

int main(int argc, char **argv)
{
	char buff[BUFSIZ];
	char *prompt =
#if ENABLE_FEATURE_SH_FANCY_PROMPT
		"\\[\\033[32;1m\\]\\u@\\[\\x1b[33;1m\\]\\h:"
		"\\[\\033[34;1m\\]\\w\\[\\033[35;1m\\] "
		"\\!\\[\\e[36;1m\\]\\$ \\[\\E[0m\\]";
#else
		"% ";
#endif

#if ENABLE_FEATURE_NONPRINTABLE_INVERSE_PUT
	setlocale(LC_ALL, "");
#endif
	while (1) {
		int l;
		l = cmdedit_read_input(prompt, buff);
		if (l <= 0 || buff[l-1] != '\n')
			break;
		buff[l-1] = 0;
		printf("*** cmdedit_read_input() returned line =%s=\n", buff);
	}
	printf("*** cmdedit_read_input() detect ^D\n");
	return 0;
}

#endif  /* TEST */
