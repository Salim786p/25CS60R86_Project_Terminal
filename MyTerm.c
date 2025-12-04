#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <termios.h>
#include <dirent.h>
#include <pwd.h>
#include <strings.h>

#define WIDTH 800
#define HEIGHT 600
#define LINE_H 20
#define MAX_LINES 1000
#define BUFSIZE 8192
#define PROMPT "user@myterm> "
#define MAX_TABS 10
#define HISTORY_FILE ".myterm_history.txt"
#define MAX_HISTORY_LINES 10000

typedef struct
{
	char *text;
} Line;

typedef struct Tab Tab;

typedef struct
{
	pid_t pid;
	char command[BUFSIZE];
} SuspendedProcess;

struct Tab
{
	char input_buf[BUFSIZE];
	int in_pos;
	Line lines[MAX_LINES];
	int line_count;
	pid_t shell_pid;
	int shell_stdin[2];
	int shell_stdout[2];
	char tab_name[32];
	int scroll_offset;
	char search_term[BUFSIZE];
	int search_pos;
	int in_search_mode;

	char *auto_complete_list[100];
	int auto_complete_count;

	volatile sig_atomic_t child_pid;

	volatile sig_atomic_t background_pids[10];
	int bg_pid_count;

	char current_directory[BUFSIZE];

	SuspendedProcess suspended_processes[10];
	int suspended_count;
};

static Tab tabs[MAX_TABS];
static int current_tab = 0;
static int tab_count = 1;

static Display *dpy;
static Window win;
static GC gc;
static XFontStruct *font;
static Colormap colormap;
static XColor green_color, white_color;

/* -------------------- Function Declarations -------------------- */
static void add_line_to_tab(Tab *tab, const char *s);
static void add_line_to_current_tab(const char *s);
static void add_line(const char *s);

/* -------------------- History File Management -------------------- */
static void load_history()
{
	char hist_path[BUFSIZE];
	snprintf(hist_path, sizeof(hist_path), "./%s", HISTORY_FILE);
	FILE *f = fopen(hist_path, "a");
	if (f)
		fclose(f);
}

static void save_to_history(const char *cmd)
{
	if (!cmd || strlen(cmd) == 0)
		return;

	char hist_path[BUFSIZE];
	snprintf(hist_path, sizeof(hist_path), "./%s", HISTORY_FILE);

	FILE *file = fopen(hist_path, "a");
	if (!file)
		return;

	fprintf(file, "%s\n", cmd);
	fclose(file);
}

static void show_history_from_file(Tab *tab)
{
	char hist_path[BUFSIZE];
	snprintf(hist_path, sizeof(hist_path), "./%s", HISTORY_FILE);

	FILE *file = fopen(hist_path, "r");
	if (!file)
	{
		add_line_to_tab(tab, "No history found");
		return;
	}

	// Count total lines
	int total_lines = 0;
	char line[BUFSIZE];
	while (fgets(line, sizeof(line), file))
		total_lines++;
	fclose(file);

	// Reopen and show last 1000 lines
	file = fopen(hist_path, "r");
	if (!file)
		return;

	int start_line = (total_lines > 1000) ? total_lines - 1000 : 0;
	int current_line = 0;

	while (fgets(line, sizeof(line), file))
	{
		if (current_line >= start_line)
		{
			line[strcspn(line, "\n")] = 0;
			char display_line[BUFSIZE];
			int max_line_len = sizeof(display_line) - 20; // Reserve space for line number
			if (max_line_len < 0)
				max_line_len = 0;
			snprintf(display_line, sizeof(display_line), "%5d  %.*s", current_line + 1, max_line_len, line);
			add_line_to_tab(tab, display_line);
		}
		current_line++;
	}
	fclose(file);
}

static void search_in_history(const char *term, Tab *tab)
{
	char hist_path[BUFSIZE];
	snprintf(hist_path, sizeof(hist_path), "./%s", HISTORY_FILE);

	FILE *file = fopen(hist_path, "r");
	if (!file)
	{
		add_line_to_tab(tab, "No history found - history file doesn't exist yet");
		return;
	}

	// Read all lines into an array for reverse search
	char lines[1000][BUFSIZE];
	int line_count = 0;

	while (fgets(lines[line_count], sizeof(lines[line_count]), file) && line_count < 1000)
	{
		lines[line_count][strcspn(lines[line_count], "\n")] = 0; // Remove newline
		line_count++;
	}
	fclose(file);

	if (line_count == 0)
	{
		add_line_to_tab(tab, "No commands in history yet");
		return;
	}

	// Search from most recent to oldest (reverse order)
	char *matches[100];
	int match_count = 0;

	for (int i = line_count - 1; i >= 0 && match_count < 100; i--)
	{
		if (strstr(lines[i], term) != NULL)
		{
			matches[match_count++] = strdup(lines[i]);
		}
	}

	if (match_count == 0)
	{
		add_line_to_tab(tab, "No match found in history");
	}
	else if (match_count == 1)
	{
		strcpy(tab->input_buf, matches[0]);
		tab->in_pos = strlen(tab->input_buf);
		free(matches[0]);
		add_line_to_tab(tab, "Command found");
	}
	else
	{
		add_line_to_tab(tab, "Multiple matches found (using most recent):");
		for (int i = 0; i < match_count && i < 10; i++)
		{
			char display_line[BUFSIZE];
			// FIXED: Clean each displayed line
			char clean_match[BUFSIZE];
			strncpy(clean_match, matches[i], sizeof(clean_match) - 1);
			clean_match[sizeof(clean_match) - 1] = '\0';
			snprintf(display_line, sizeof(display_line), "  %d. %.*s", i + 1, (int)sizeof(display_line) - 20, clean_match);
			add_line_to_tab(tab, display_line);
		}
		strcpy(tab->input_buf, matches[0]);
		tab->in_pos = strlen(tab->input_buf);
		free(matches[0]);
	}

	// Free remaining matches
	for (int i = 1; i < match_count; i++)
	{
		free(matches[i]);
	}
}

/* -------------------- GUI Drawing -------------------- */
static void add_line_to_tab(Tab *tab, const char *s)
{
	if (tab->line_count < MAX_LINES)
	{
		tab->lines[tab->line_count].text = strdup(s);
		tab->line_count++;
	}
	else
	{
		free(tab->lines[0].text);
		memmove(tab->lines, tab->lines + 1, (MAX_LINES - 1) * sizeof(Line));
		tab->lines[MAX_LINES - 1].text = strdup(s);
	}
}

static void add_line_to_current_tab(const char *s)
{
	add_line_to_tab(&tabs[current_tab], s);
}

static void add_line(const char *s)
{
	add_line_to_current_tab(s);
}

/* -------------------- Tab Management -------------------- */
static void create_new_tab()
{
	if (tab_count >= MAX_TABS)
		return;

	int new_tab = tab_count++;
	Tab *tab = &tabs[new_tab];

	tab->in_pos = 0;
	tab->input_buf[0] = '\0';
	tab->line_count = 0;
	tab->shell_pid = 0;
	tab->scroll_offset = 0;
	tab->search_term[0] = '\0';
	tab->search_pos = 0;
	tab->in_search_mode = 0;
	tab->child_pid = -1;
	tab->auto_complete_count = 0;
	tab->bg_pid_count = 0;
	tab->suspended_count = 0;

	for (int i = 0; i < 10; i++)
	{
		tab->background_pids[i] = 0;
		tab->suspended_processes[i].pid = -1;
		tab->suspended_processes[i].command[0] = '\0';
	}

	// Initialize current directory
	if (getcwd(tab->current_directory, sizeof(tab->current_directory)) == NULL)
	{
		strcpy(tab->current_directory, "/");
	}

	snprintf(tab->tab_name, sizeof(tab->tab_name), "Tab %d", new_tab + 1);

	// Add welcome message to the new tab
	char welcome[BUFSIZE];
	snprintf(welcome, sizeof(welcome), "New tab %d created - Use Ctrl+W to close tab", new_tab + 1);

	if (tab->line_count < MAX_LINES)
	{
		tab->lines[tab->line_count].text = strdup(welcome);
		tab->line_count++;
	}
}

static void close_current_tab()
{
	if (tab_count <= 1)
		return;

	Tab *tab = &tabs[current_tab];

	// Kill shell process if exists
	if (tab->shell_pid > 0)
	{
		kill(tab->shell_pid, SIGTERM);
		waitpid(tab->shell_pid, NULL, 0);
	}

	// Kill child process if exists
	if (tab->child_pid > 0)
	{
		kill(tab->child_pid, SIGTERM);
		waitpid(tab->child_pid, NULL, 0);
	}

	// Kill background processes for this tab
	for (int i = 0; i < tab->bg_pid_count; i++)
	{
		if (tab->background_pids[i] > 0)
		{
			kill(tab->background_pids[i], SIGTERM);
			waitpid(tab->background_pids[i], NULL, 0);
		}
	}

	// Kill suspended processes for this tab
	for (int i = 0; i < tab->suspended_count; i++)
	{
		if (tab->suspended_processes[i].pid > 0)
		{
			kill(tab->suspended_processes[i].pid, SIGTERM);
			waitpid(tab->suspended_processes[i].pid, NULL, 0);
		}
	}

	// Free all line texts in the tab being closed
	for (int i = 0; i < tab->line_count; i++)
	{
		free(tab->lines[i].text);
	}

	// Free auto-complete list if any
	for (int i = 0; i < tab->auto_complete_count; i++)
	{
		free(tab->auto_complete_list[i]);
	}

	// Shift tabs left
	for (int i = current_tab; i < tab_count - 1; i++)
	{
		tabs[i] = tabs[i + 1];
	}

	tab_count--;

	// Adjust current_tab if needed
	if (current_tab >= tab_count)
	{
		current_tab = tab_count - 1;
	}

	// Add close message to current tab
	char msg[64];
	snprintf(msg, sizeof(msg), "Tab closed. Now in %s", tabs[current_tab].tab_name);
	add_line_to_tab(&tabs[current_tab], msg);
}

static void switch_tab(int direction)
{
	if (tab_count <= 1)
		return;

	current_tab = (current_tab + direction + tab_count) % tab_count;

	char msg[64];
	snprintf(msg, sizeof(msg), "Switched to %s", tabs[current_tab].tab_name);

	Tab *tab = &tabs[current_tab];
	if (tab->line_count < MAX_LINES)
	{
		tab->lines[tab->line_count].text = strdup(msg);
		tab->line_count++;
	}
}

static void redraw()
{
	XClearWindow(dpy, win);
	int tab_width = WIDTH / tab_count;
	for (int i = 0; i < tab_count; i++)
	{
		int x = i * tab_width;
		if (i == current_tab)
		{
			// Highlight current tab
			XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
			XFillRectangle(dpy, win, gc, x, 0, tab_width, LINE_H);
			XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
		}
		else
		{
			XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
			XSetBackground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
		}
		XDrawString(dpy, win, gc, x + 4, LINE_H - 4, tabs[i].tab_name, strlen(tabs[i].tab_name));
		XDrawRectangle(dpy, win, gc, x, 0, tab_width, LINE_H);
	}

	// Reset background for content area
	XSetBackground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
	XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));

	// Draw content of current tab with scroll support
	Tab *tab = &tabs[current_tab];
	int y = LINE_H * 2;

	// Calculate visible lines based on scroll offset
	int visible_lines = (HEIGHT - (LINE_H * 3)) / LINE_H;
	int start_line = 0;

	if (tab->line_count > visible_lines)
	{
		start_line = tab->line_count - visible_lines - tab->scroll_offset;
		if (start_line < 0)
			start_line = 0;
		if (start_line > tab->line_count - visible_lines)
			start_line = tab->line_count - visible_lines;
	}

	// Draw scroll indicator if needed
	if (tab->line_count > visible_lines)
	{
		char scroll_info[64];
		snprintf(scroll_info, sizeof(scroll_info), "Lines: %d-%d/%d (Use Up/Down to scroll)",
				 start_line + 1, start_line + visible_lines, tab->line_count);
		XDrawString(dpy, win, gc, 4, LINE_H * 1.5, scroll_info, strlen(scroll_info));
	}

	// Draw visible lines
	for (int i = start_line; i < tab->line_count && (i - start_line) < visible_lines; i++)
	{
		XDrawString(dpy, win, gc, 4, y, tab->lines[i].text, strlen(tab->lines[i].text));
		y += LINE_H;
	}

	// If in search mode, show search prompt
	if (tab->in_search_mode)
	{
		char search_prompt[BUFSIZE + 50];
		snprintf(search_prompt, sizeof(search_prompt), "Search: %s", tab->search_term);
		XDrawString(dpy, win, gc, 4, HEIGHT - LINE_H, search_prompt, strlen(search_prompt));

		int search_text_width = XTextWidth(font, "Search: ", strlen("Search: "));
		int search_term_width = XTextWidth(font, tab->search_term, strlen(tab->search_term));
		int cursor_x = 4 + search_text_width + search_term_width;
		XFillRectangle(dpy, win, gc, cursor_x, HEIGHT - LINE_H + 2, 8, 2);
	}
	else
	{
		char promptline[BUFSIZE * 2];
		snprintf(promptline, sizeof(promptline), "%s", PROMPT);

		// Draw prompt in green color
		XSetForeground(dpy, gc, green_color.pixel);
		XDrawString(dpy, win, gc, 4, HEIGHT - LINE_H, promptline, strlen(promptline));
		XSetForeground(dpy, gc, white_color.pixel);

		// Draw multiline input with proper line breaks
		int prompt_width = XTextWidth(font, PROMPT, strlen(PROMPT));
		int current_y = HEIGHT - LINE_H;
		char *input_ptr = tab->input_buf;
		int current_pos = 0;
		int cursor_drawn = 0;

		while (*input_ptr)
		{
			char line_buf[BUFSIZE];
			int line_len = 0;

			while (*input_ptr && *input_ptr != '\n' && line_len < BUFSIZE - 1)
			{
				line_buf[line_len++] = *input_ptr++;
			}
			line_buf[line_len] = '\0';

			XDrawString(dpy, win, gc, 4 + prompt_width, current_y, line_buf, line_len);

			if (!cursor_drawn && current_pos + line_len >= tab->in_pos)
			{
				int cursor_pos_in_line = tab->in_pos - current_pos;
				if (cursor_pos_in_line < 0)
					cursor_pos_in_line = 0;
				if (cursor_pos_in_line > line_len)
					cursor_pos_in_line = line_len;

				int cursor_x = 4 + prompt_width + XTextWidth(font, line_buf, cursor_pos_in_line);
				XFillRectangle(dpy, win, gc, cursor_x, current_y + 2, 8, 2);
				cursor_drawn = 1;
			}

			current_pos += line_len;

			if (*input_ptr == '\n')
			{
				input_ptr++;
				current_pos++;
				current_y += LINE_H;
				prompt_width = 0;
			}
		}

		if (!cursor_drawn)
		{
			int cursor_x = 4 + prompt_width + XTextWidth(font, input_ptr, strlen(input_ptr));
			XFillRectangle(dpy, win, gc, cursor_x, current_y + 2, 8, 2);
		}
	}
}

// ---------- multiWatch implementation ----------
static volatile sig_atomic_t mw_stop_flag = 0;

static void mw_sigint(int s)
{
	(void)s;
	mw_stop_flag = 1;
}

static char *trim_spaces(char *s)
{
	if (!s)
		return s;
	while (*s == ' ' || *s == '\t')
		s++;
	char *end = s + strlen(s) - 1;
	while (end >= s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
	{
		*end = '\0';
		end--;
	}
	return s;
}

static int parse_multiwatch_list(char *arg, char **commands, int maxcmds)
{
	char *l = strchr(arg, '[');
	if (!l)
		return 0;
	char *r = strchr(l, ']');
	if (!r)
		return 0;
	*r = '\0';
	l++;

	int n = 0;
	char *p = l;

	while (*p && n < maxcmds)
	{
		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		if (!*p)
			break;

		if (*p == '"')
		{
			p++;
			char *start = p;
			while (*p && *p != '"')
			{
				if (*p == '\\' && *(p + 1))
					p++;
				p++;
			}
			if (*p == '"')
			{
				*p = '\0';
				commands[n++] = start;
				p++;
			}
			else
			{
				commands[n++] = start;
				break;
			}
		}
		else
		{
			char *start = p;
			while (*p && *p != ',')
				p++;
			if (*p == ',')
			{
				*p = '\0';
				commands[n++] = trim_spaces(start);
				p++;
			}
			else
			{
				commands[n++] = trim_spaces(start);
				break;
			}
		}
	}
	return n;
}

static void multiWatch_runner(Tab *tab, const char *argline)
{
	char local[BUFSIZE];
	strncpy(local, argline, sizeof(local) - 1);
	local[sizeof(local) - 1] = 0;

	char *listpart = strchr(local, '[');
	if (!listpart)
	{
		add_line_to_tab(tab, "multiWatch: malformed arguments (expected [ ... ])");
		return;
	}

#define MAX_MW_CMDS 64
	char *commands[MAX_MW_CMDS];
	int ncmd = parse_multiwatch_list(listpart, commands, MAX_MW_CMDS);

	if (ncmd <= 0)
	{
		add_line_to_tab(tab, "multiWatch: no commands found");
		return;
	}

	add_line_to_tab(tab, "multiWatch: starting parallel execution");

	pid_t pids[MAX_MW_CMDS];
	char tempfiles[MAX_MW_CMDS][256];
	int fds[MAX_MW_CMDS];

	struct sigaction old_act, new_act;
	mw_stop_flag = 0;
	memset(&new_act, 0, sizeof(new_act));
	new_act.sa_handler = mw_sigint;
	sigemptyset(&new_act.sa_mask);
	sigaction(SIGINT, &new_act, &old_act);

	for (int i = 0; i < ncmd; i++)
	{
		pid_t pid = fork();
		if (pid == 0)
		{
			if (chdir(tab->current_directory) == -1)
			{
				chdir(getenv("HOME") ? getenv("HOME") : "/");
			}

			pid_t mypid = getpid();
			snprintf(tempfiles[i], sizeof(tempfiles[i]), "/tmp/.temp_mw_%d_%d.txt", (int)getppid(), (int)mypid);
			int fd = open(tempfiles[i], O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd < 0)
			{
				perror("open");
				_exit(1);
			}
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
			execl("/bin/bash", "bash", "-c", commands[i], (char *)NULL);
			perror("execl");
			_exit(127);
		}
		else if (pid > 0)
		{
			pids[i] = pid;
			tab->child_pid = pid;
			snprintf(tempfiles[i], sizeof(tempfiles[i]), "/tmp/.temp_mw_%d_%d.txt", (int)getpid(), (int)pid);
		}
		else
		{
			perror("fork");
			return;
		}
	}

	sleep(1);

	for (int i = 0; i < ncmd; i++)
	{
		fds[i] = open(tempfiles[i], O_RDONLY | O_NONBLOCK);
		if (fds[i] < 0)
		{
			char err[256];
			snprintf(err, sizeof(err), "Warning: Could not open temp file for PID %d", (int)pids[i]);
			add_line_to_tab(tab, err);
		}
	}

	struct pollfd pfds[MAX_MW_CMDS];
	for (int i = 0; i < ncmd; i++)
	{
		pfds[i].fd = fds[i];
		pfds[i].events = (fds[i] >= 0) ? POLLIN : 0;
	}

	char buf[BUFSIZE];
	off_t file_pos[MAX_MW_CMDS] = {0};

	while (!mw_stop_flag)
	{
		int rc = poll(pfds, ncmd, 100);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		int alive_children = 0;
		for (int i = 0; i < ncmd; i++)
		{
			if (pids[i] > 0)
			{
				if (waitpid(pids[i], NULL, WNOHANG) == 0)
				{
					alive_children++;
				}
				else
				{
					pids[i] = 0;
				}
			}
		}

		int data_processed = 0;
		for (int i = 0; i < ncmd; i++)
		{
			if (pfds[i].fd >= 0 && (pfds[i].revents & POLLIN))
			{
				lseek(fds[i], file_pos[i], SEEK_SET);
				ssize_t r = read(fds[i], buf, sizeof(buf) - 1);
				if (r > 0)
				{
					buf[r] = '\0';
					file_pos[i] += r;
					data_processed = 1;
					time_t now = time(NULL);
					char ts[64];
					strftime(ts, sizeof(ts), "%F %T", localtime(&now));
					char header[256];
					snprintf(header, sizeof(header), "\"%s\", %s:", commands[i], ts);
					add_line_to_tab(tab, header);
					add_line_to_tab(tab, "----------------------------------------------------");
					char *line_start = buf;
					char *line_end;
					while ((line_end = strchr(line_start, '\n')) != NULL)
					{
						int line_len = line_end - line_start;
						if (line_len > 0)
						{
							char line[BUFSIZE];
							strncpy(line, line_start, line_len);
							line[line_len] = '\0';
							add_line_to_tab(tab, line);
						}
						line_start = line_end + 1;
					}
					add_line_to_tab(tab, "----------------------------------------------------");
					redraw();
				}
				else if (r == 0)
				{
					close(fds[i]);
					pfds[i].fd = -1;
				}
			}
			if (pfds[i].fd >= 0 && (pfds[i].revents & POLLHUP))
			{
				close(fds[i]);
				pfds[i].fd = -1;
			}
		}

		if (alive_children == 0 && !data_processed)
		{
			break;
		}
	}

	for (int i = 0; i < ncmd; i++)
	{
		if (pids[i] > 0)
		{
			kill(pids[i], SIGKILL);
			waitpid(pids[i], NULL, 0);
		}
		if (fds[i] >= 0)
			close(fds[i]);
		unlink(tempfiles[i]);
	}

	sigaction(SIGINT, &old_act, NULL);
	tab->child_pid = -1;
	add_line_to_tab(tab, "multiWatch: execution completed");
}

// Line Navigation
static void move_cursor_start(Tab *tab)
{
	tab->in_pos = 0;
}

static void move_cursor_end(Tab *tab)
{
	tab->in_pos = strlen(tab->input_buf);
}

// Signal Handling
static void sigint_handler(int sig)
{
	Tab *tab = &tabs[current_tab];
	if (tab->child_pid > 0)
	{
		kill(tab->child_pid, SIGINT);
		add_line_to_tab(tab, "^C");
		redraw();
		tab->child_pid = -1;
	}
}

static void sigtstp_handler(int sig)
{
	Tab *tab = &tabs[current_tab];
	if (tab->child_pid > 0)
	{
		kill(tab->child_pid, SIGTSTP);

		// Store the suspended process with its command
		if (tab->suspended_count < 10)
		{
			tab->suspended_processes[tab->suspended_count].pid = tab->child_pid;
			// Store the last command that was executed
			if (strlen(tab->input_buf) > 0)
			{
				strncpy(tab->suspended_processes[tab->suspended_count].command,
						tab->input_buf,
						sizeof(tab->suspended_processes[tab->suspended_count].command) - 1);
				tab->suspended_processes[tab->suspended_count].command[sizeof(tab->suspended_processes[tab->suspended_count].command) - 1] = '\0';
			}
			else
			{
				strcpy(tab->suspended_processes[tab->suspended_count].command, "unknown");
			}
			tab->suspended_count++;

			char msg[256];
			snprintf(msg, sizeof(msg), "[%d] suspended", tab->child_pid);
			add_line_to_tab(tab, msg);
		}

		add_line_to_tab(tab, "^Z");
		redraw();
		tab->child_pid = -1;
	}
}

static void find_matching_files(Tab *tab, const char *prefix)
{
	tab->auto_complete_count = 0;
	DIR *dir = opendir(".");
	if (!dir)
		return;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL && tab->auto_complete_count < 100)
	{
		if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0)
		{
			tab->auto_complete_list[tab->auto_complete_count++] = strdup(entry->d_name);
		}
	}
	closedir(dir);
}

static char *find_common_prefix(Tab *tab)
{
	if (tab->auto_complete_count == 0)
		return NULL;
	int min_len = strlen(tab->auto_complete_list[0]);
	for (int i = 1; i < tab->auto_complete_count; i++)
	{
		int len = strlen(tab->auto_complete_list[i]);
		if (len < min_len)
			min_len = len;
	}
	for (int pos = 0; pos < min_len; pos++)
	{
		char c = tab->auto_complete_list[0][pos];
		for (int i = 1; i < tab->auto_complete_count; i++)
		{
			if (tab->auto_complete_list[i][pos] != c)
			{
				char *prefix = malloc(pos + 1);
				strncpy(prefix, tab->auto_complete_list[0], pos);
				prefix[pos] = '\0';
				return prefix;
			}
		}
	}
	char *prefix = malloc(min_len + 1);
	strncpy(prefix, tab->auto_complete_list[0], min_len);
	prefix[min_len] = '\0';
	return prefix;
}

static void handle_auto_complete(Tab *tab)
{
	// Find the current word being typed
	char current_word[BUFSIZE] = {0};
	int word_start = tab->in_pos;

	// Find the start of the current word
	while (word_start > 0 && tab->input_buf[word_start - 1] != ' ' &&
		   tab->input_buf[word_start - 1] != '\n' &&
		   tab->input_buf[word_start - 1] != '|' &&
		   tab->input_buf[word_start - 1] != '>' &&
		   tab->input_buf[word_start - 1] != '<' &&
		   tab->input_buf[word_start - 1] != '&')
	{
		word_start--;
	}

	int word_len = tab->in_pos - word_start;

	if (word_len <= 0)
	{
		// If no word to complete, show all files
		strcpy(current_word, "");
	}
	else
	{
		strncpy(current_word, tab->input_buf + word_start, word_len);
		current_word[word_len] = '\0';
	}

	// Clear previous auto-complete list
	for (int i = 0; i < tab->auto_complete_count; i++)
	{
		free(tab->auto_complete_list[i]);
	}
	tab->auto_complete_count = 0;

	// Find matching files in current directory
	DIR *dir = opendir(tab->current_directory);
	if (!dir)
	{
		// Fallback to current working directory if tab directory fails
		dir = opendir(".");
	}

	if (!dir)
	{
		add_line_to_tab(tab, "Error: Cannot open directory for auto-completion");
		return;
	}

	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL && tab->auto_complete_count < 100)
	{
		// Skip hidden files unless the current word starts with '.'
		if (entry->d_name[0] == '.' && current_word[0] != '.')
		{
			continue;
		}

		// Check if this entry matches our current word
		if (strncmp(entry->d_name, current_word, word_len) == 0)
		{
			tab->auto_complete_list[tab->auto_complete_count++] = strdup(entry->d_name);
		}
	}
	closedir(dir);

	if (tab->auto_complete_count == 0)
	{
		// No matches found
		return;
	}
	else if (tab->auto_complete_count == 1)
	{
		// Single match - complete it
		char *match = tab->auto_complete_list[0];
		int match_len = strlen(match);
		int remaining_len = match_len - word_len;

		if (remaining_len > 0 && tab->in_pos + remaining_len < BUFSIZE - 1)
		{
			// Insert the remaining part of the filename
			memmove(tab->input_buf + tab->in_pos + remaining_len,
					tab->input_buf + tab->in_pos,
					strlen(tab->input_buf + tab->in_pos) + 1);

			memcpy(tab->input_buf + tab->in_pos, match + word_len, remaining_len);
			tab->in_pos += remaining_len;

			// Add a space if it's a complete word (not a partial path)
			if (tab->in_pos < BUFSIZE - 1)
			{
				tab->input_buf[tab->in_pos] = ' ';
				tab->in_pos++;
				tab->input_buf[tab->in_pos] = '\0';
			}
		}

		free(tab->auto_complete_list[0]);
		tab->auto_complete_count = 0;
	}
	else
	{
		// Multiple matches
		char *common_prefix = find_common_prefix(tab);

		if (common_prefix && strlen(common_prefix) > word_len)
		{
			// Complete to common prefix
			int common_len = strlen(common_prefix);
			int remaining_len = common_len - word_len;

			if (remaining_len > 0 && tab->in_pos + remaining_len < BUFSIZE - 1)
			{
				memmove(tab->input_buf + tab->in_pos + remaining_len,
						tab->input_buf + tab->in_pos,
						strlen(tab->input_buf + tab->in_pos) + 1);

				memcpy(tab->input_buf + tab->in_pos, common_prefix + word_len, remaining_len);
				tab->in_pos += remaining_len;
				tab->input_buf[tab->in_pos] = '\0';
			}

			free(common_prefix);
		}
		else
		{
			add_line_to_tab(tab, "Multiple matches:");

			for (int i = 0; i < tab->auto_complete_count - 1; i++)
			{
				for (int j = i + 1; j < tab->auto_complete_count; j++)
				{
					if (strcasecmp(tab->auto_complete_list[i], tab->auto_complete_list[j]) > 0)
					{
						char *temp = tab->auto_complete_list[i];
						tab->auto_complete_list[i] = tab->auto_complete_list[j];
						tab->auto_complete_list[j] = temp;
					}
				}
			}

			int max_per_line = 2;
			int items_per_col = (tab->auto_complete_count + max_per_line - 1) / max_per_line;

			for (int row = 0; row < items_per_col; row++)
			{
				char line[BUFSIZE] = "";
				int line_len = 0;

				for (int col = 0; col < max_per_line; col++)
				{
					int index = row + col * items_per_col;
					if (index < tab->auto_complete_count)
					{
						char item[256];
						// Add indicator for directories
						char full_path[BUFSIZE];
						// Safe path construction - use filename only to avoid path length issues
						strncpy(full_path, tab->auto_complete_list[index], sizeof(full_path) - 1);
						full_path[sizeof(full_path) - 1] = '\0';

						struct stat st;
						int is_dir = 0;
						if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode))
						{
							is_dir = 1;
						}

						if (is_dir)
						{
							snprintf(item, sizeof(item), "%-30s/", tab->auto_complete_list[index]);
						}
						else
						{
							snprintf(item, sizeof(item), "%-30s", tab->auto_complete_list[index]);
						}

						// Check if we have space in the line
						if (line_len + strlen(item) < sizeof(line) - 10)
						{
							strcat(line, item);
							line_len += strlen(item);
						}
					}
				}

				if (strlen(line) > 0)
				{
					add_line_to_tab(tab, line);
				}
			}

			if (common_prefix)
				free(common_prefix);
		}

		// Clean up auto-complete list
		for (int i = 0; i < tab->auto_complete_count; i++)
		{
			free(tab->auto_complete_list[i]);
		}
		tab->auto_complete_count = 0;
	}
}

/* -------------------- Command Execution -------------------- */
static void execute_command(Tab *tab, const char *cmdline)
{
	if (!cmdline || strlen(cmdline) == 0)
		return;

	// Save to file-based history
	save_to_history(cmdline);

	if (strcmp(cmdline, "history") == 0)
	{
		show_history_from_file(tab);
		return;
	}

	if (strncmp(cmdline, "cd", 2) == 0 && (cmdline[2] == ' ' || cmdline[2] == '\0'))
	{
		const char *path = cmdline + 2;
		while (*path == ' ')
			path++;
		if (*path == '\0')
		{
			path = getenv("HOME");
			if (!path)
				path = "/";
		}

		// Store the current directory before changing
		char old_cwd[BUFSIZE];
		if (getcwd(old_cwd, sizeof(old_cwd)) == NULL)
		{
			strcpy(old_cwd, "unknown");
		}

		if (chdir(path) == -1)
		{
			char errbuf[256];
			snprintf(errbuf, sizeof(errbuf), "cd: %s", strerror(errno));
			add_line_to_tab(tab, errbuf);
		}
		else
		{
			// Update tab's current directory
			if (getcwd(tab->current_directory, sizeof(tab->current_directory)) == NULL)
			{
				strcpy(tab->current_directory, path); // Fallback
			}
			char okbuf[256];
			int len = strlen(tab->current_directory);
			if (len > 200)
			{
				// Truncate very long directory names
				snprintf(okbuf, sizeof(okbuf), "[changed directory to ...%.200s]", tab->current_directory + (len - 200));
			}
			else
			{
				snprintf(okbuf, sizeof(okbuf), "[changed directory to %.200s]", tab->current_directory);
			}
			add_line_to_tab(tab, okbuf);
		}
		return;
	}

	if (strcmp(cmdline, "fg") == 0)
	{
		if (tab->suspended_count == 0)
		{
			add_line_to_tab(tab, "fg: no suspended jobs");
			return;
		}

		// Get the most recently suspended process
		int last_index = tab->suspended_count - 1;
		pid_t pid_to_resume = tab->suspended_processes[last_index].pid;

		if (kill(pid_to_resume, SIGCONT) == -1)
		{
			char errbuf[256];
			snprintf(errbuf, sizeof(errbuf), "fg: cannot resume [%d]: %s", pid_to_resume, strerror(errno));
			add_line_to_tab(tab, errbuf);
			// Remove the process from suspended list since it's gone
			tab->suspended_count--;
			return;
		}

		char msg[256];
		// Truncate long commands for display
		char truncated_cmd[200];
		strncpy(truncated_cmd, tab->suspended_processes[last_index].command, sizeof(truncated_cmd) - 1);
		truncated_cmd[sizeof(truncated_cmd) - 1] = '\0';
		if (strlen(tab->suspended_processes[last_index].command) > sizeof(truncated_cmd) - 1)
		{
			strcpy(truncated_cmd + sizeof(truncated_cmd) - 4, "...");
		}
		snprintf(msg, sizeof(msg), "Resumed [%d]: %s", pid_to_resume, truncated_cmd);
		add_line_to_tab(tab, msg);

		// Set as current child process
		tab->child_pid = pid_to_resume;

		// Remove from suspended list
		tab->suspended_count--;

		// Wait for the resumed process
		int status;
		pid_t result = waitpid(pid_to_resume, &status, 0);
		if (result > 0)
		{
			if (WIFEXITED(status))
			{
				snprintf(msg, sizeof(msg), "[%d] exited with status %d", pid_to_resume, WEXITSTATUS(status));
				add_line_to_tab(tab, msg);
			}
			else if (WIFSIGNALED(status))
			{
				snprintf(msg, sizeof(msg), "[%d] killed by signal %d", pid_to_resume, WTERMSIG(status));
				add_line_to_tab(tab, msg);
			}
		}
		tab->child_pid = -1;
		return;
	}

	if (strcmp(cmdline, "jobs") == 0)
	{
		if (tab->suspended_count == 0)
		{
			add_line_to_tab(tab, "No suspended jobs");
			return;
		}

		add_line_to_tab(tab, "Suspended jobs:");
		for (int i = 0; i < tab->suspended_count; i++)
		{
			char job_info[256];
			// Truncate long commands for display
			char truncated_cmd[200];
			strncpy(truncated_cmd, tab->suspended_processes[i].command, sizeof(truncated_cmd) - 1);
			truncated_cmd[sizeof(truncated_cmd) - 1] = '\0';
			if (strlen(tab->suspended_processes[i].command) > sizeof(truncated_cmd) - 1)
			{
				strcpy(truncated_cmd + sizeof(truncated_cmd) - 4, "...");
			}
			snprintf(job_info, sizeof(job_info), "[%d] %d %s",
					 i + 1,
					 tab->suspended_processes[i].pid,
					 truncated_cmd);
			add_line_to_tab(tab, job_info);
		}
		return;
	}

	if (strncmp(cmdline, "multiWatch", 10) == 0)
	{
		const char *args = cmdline + 10;
		while (*args == ' ')
			args++;
		multiWatch_runner(tab, args);
		return;
	}

	if (strcmp(cmdline, "exit") == 0)
	{
		add_line_to_tab(tab, "Closing MyTerm...");
		redraw();
		sleep(1);
		XCloseDisplay(dpy);
		exit(0);
	}

	// Check for background process (ending with &)
	int run_in_background = 0;
	char clean_cmdline[BUFSIZE];
	strncpy(clean_cmdline, cmdline, sizeof(clean_cmdline) - 1);
	clean_cmdline[sizeof(clean_cmdline) - 1] = '\0';

	// Remove trailing spaces and check for &
	char *end = clean_cmdline + strlen(clean_cmdline) - 1;
	while (end > clean_cmdline && (*end == ' ' || *end == '\t'))
	{
		end--;
	}
	if (end > clean_cmdline && *end == '&')
	{
		run_in_background = 1;
		*end = '\0';
		// Remove any spaces before the &
		end--;
		while (end > clean_cmdline && (*end == ' ' || *end == '\t'))
		{
			*end = '\0';
			end--;
		}
	}

	// Pipes
	if (strchr(clean_cmdline, '|'))
	{
		char pipeline[BUFSIZE];
		strncpy(pipeline, clean_cmdline, sizeof(pipeline) - 1);
		pipeline[sizeof(pipeline) - 1] = 0;
		char *commands[64];
		int ncmd = 0;
		char *tok = strtok(pipeline, "|");
		while (tok && ncmd < 64)
		{
			while (*tok == ' ' || *tok == '\t')
				tok++;
			char *end = tok + strlen(tok) - 1;
			while (end >= tok && (*end == ' ' || *end == '\t' || *end == '\n'))
			{
				*end = '\0';
				end--;
			}
			commands[ncmd++] = tok;
			tok = strtok(NULL, "|");
		}
		if (ncmd == 0)
			return;

		int pipes[64][2];
		for (int i = 0; i < ncmd - 1; ++i)
		{
			if (pipe(pipes[i]) == -1)
			{
				perror("pipe");
				return;
			}
		}
		int gui_pipe[2];
		if (pipe(gui_pipe) == -1)
		{
			perror("pipe gui");
			for (int i = 0; i < ncmd - 1; ++i)
			{
				close(pipes[i][0]);
				close(pipes[i][1]);
			}
			return;
		}

		pid_t pids[64];
		for (int i = 0; i < ncmd; ++i)
		{
			pid_t pid = fork();
			if (pid == 0)
			{
				// Child process - set the current directory for this tab
				if (chdir(tab->current_directory) == -1)
				{
					// If we can't change to the tab's directory, fall back to home
					chdir(getenv("HOME") ? getenv("HOME") : "/");
				}

				if (i > 0)
				{
					dup2(pipes[i - 1][0], STDIN_FILENO);
				}
				else
				{
					int devnull = open("/dev/null", O_RDONLY);
					if (devnull >= 0)
					{
						dup2(devnull, STDIN_FILENO);
						close(devnull);
					}
				}
				if (i < ncmd - 1)
				{
					dup2(pipes[i][1], STDOUT_FILENO);
				}
				else
				{
					dup2(gui_pipe[1], STDOUT_FILENO);
					dup2(gui_pipe[1], STDERR_FILENO);
				}
				for (int j = 0; j < ncmd - 1; ++j)
				{
					close(pipes[j][0]);
					close(pipes[j][1]);
				}
				close(gui_pipe[0]);
				close(gui_pipe[1]);
				execl("/bin/bash", "bash", "-c", commands[i], (char *)NULL);
				perror("execl");
				_exit(127);
			}
			else if (pid > 0)
			{
				pids[i] = pid;
				if (!run_in_background)
				{
					tab->child_pid = pid;
				}
				else
				{
					// Add to background processes
					if (tab->bg_pid_count < 10)
					{
						tab->background_pids[tab->bg_pid_count++] = pid;
						char msg[256];
						snprintf(msg, sizeof(msg), "[%d] running in background", pid);
						add_line_to_tab(tab, msg);
					}
				}
			}
			else
			{
				perror("fork");
				return;
			}
		}
		for (int i = 0; i < ncmd - 1; ++i)
		{
			close(pipes[i][0]);
			close(pipes[i][1]);
		}
		close(gui_pipe[1]);

		if (!run_in_background)
		{
			// Read output from the pipe and display it
			char buf[BUFSIZE];
			ssize_t r;
			while ((r = read(gui_pipe[0], buf, sizeof(buf) - 1)) > 0)
			{
				buf[r] = '\0';
				char *line_start = buf;
				char *line_end;
				while ((line_end = strchr(line_start, '\n')) != NULL)
				{
					int line_len = line_end - line_start;
					if (line_len > 0)
					{
						char line[BUFSIZE];
						strncpy(line, line_start, line_len);
						line[line_len] = '\0';
						add_line_to_tab(tab, line);
					}
					line_start = line_end + 1;
				}
				// Handle any remaining data without newline
				if (line_start < buf + r)
				{
					int remaining_len = (buf + r) - line_start;
					if (remaining_len > 0)
					{
						char line[BUFSIZE];
						strncpy(line, line_start, remaining_len);
						line[remaining_len] = '\0';
						add_line_to_tab(tab, line);
					}
				}
				redraw();
			}
			close(gui_pipe[0]);
			for (int i = 0; i < ncmd; ++i)
			{
				waitpid(pids[i], NULL, 0);
			}
			tab->child_pid = -1;
		}
		else
		{
			close(gui_pipe[0]);
			// Don't wait for background processes
		}
		return;
	}

	// Handle redirection and regular commands with proper output capture
	char *input_file = NULL;
	char *output_file = NULL;
	char cmd_copy[BUFSIZE];
	strncpy(cmd_copy, clean_cmdline, sizeof(cmd_copy) - 1);
	cmd_copy[sizeof(cmd_copy) - 1] = 0;

	char *input_redir = strchr(cmd_copy, '<');
	char *output_redir = strchr(cmd_copy, '>');
	if (input_redir)
	{
		*input_redir = '\0';
		input_file = input_redir + 1;
		while (*input_file == ' ')
			input_file++;
		char *end = input_file + strlen(input_file) - 1;
		while (end >= input_file && (*end == ' ' || *end == '\n' || *end == '\r'))
		{
			*end = '\0';
			end--;
		}
	}
	if (output_redir)
	{
		*output_redir = '\0';
		output_file = output_redir + 1;
		while (*output_file == ' ')
			output_file++;
		char *end = output_file + strlen(output_file) - 1;
		while (end >= output_file && (*end == ' ' || *end == '\n' || *end == '\r'))
		{
			*end = '\0';
			end--;
		}
	}

	char *trimmed_cmd = cmd_copy;
	while (*trimmed_cmd == ' ')
		trimmed_cmd++;
	char *end_cmd = trimmed_cmd + strlen(trimmed_cmd) - 1;
	while (end_cmd >= trimmed_cmd && (*end_cmd == ' ' || *end_cmd == '\n' || *end_cmd == '\r'))
	{
		*end_cmd = '\0';
		end_cmd--;
	}

	if (strlen(trimmed_cmd) == 0)
	{
		if (input_file || output_file)
		{
			add_line_to_tab(tab, "Error: Redirection specified but no command given");
		}
		return;
	}

	int output_pipe[2];
	if (pipe(output_pipe) == -1)
	{
		perror("pipe");
		return;
	}

	pid_t pid = fork();
	if (pid == 0)
	{
		// set the current directory for this tab
		if (chdir(tab->current_directory) == -1)
		{
			// If we can't change to the tab's directory, fall back to home
			chdir(getenv("HOME") ? getenv("HOME") : "/");
		}

		close(output_pipe[0]);

		if (input_file)
		{
			int fd_in = open(input_file, O_RDONLY);
			if (fd_in == -1)
			{
				perror("open input file");
				_exit(1);
			}
			dup2(fd_in, STDIN_FILENO);
			close(fd_in);
		}

		if (output_file)
		{
			int fd_out = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd_out == -1)
			{
				perror("open output file");
				_exit(1);
			}
			dup2(fd_out, STDOUT_FILENO);
			dup2(fd_out, STDERR_FILENO);
			close(fd_out);
		}
		else
		{
			// Redirect stdout and stderr to the pipe
			dup2(output_pipe[1], STDOUT_FILENO);
			dup2(output_pipe[1], STDERR_FILENO);
		}

		close(output_pipe[1]);

		execl("/bin/bash", "bash", "-c", trimmed_cmd, (char *)NULL);
		perror("execl");
		_exit(127);
	}
	else if (pid > 0)
	{
		// Parent process
		close(output_pipe[1]);

		if (run_in_background)
		{
			// Add to background processes
			if (tab->bg_pid_count < 10)
			{
				tab->background_pids[tab->bg_pid_count++] = pid;
				char msg[256];
				snprintf(msg, sizeof(msg), "[%d] running in background", pid);
				add_line_to_tab(tab, msg);
			}
			close(output_pipe[0]);
		}
		else
		{
			tab->child_pid = pid;

			// Set the output pipe to non-blocking mode
			int flags = fcntl(output_pipe[0], F_GETFL, 0);
			fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);

			// Use poll to monitor the pipe with timeout
			struct pollfd fds[1];
			fds[0].fd = output_pipe[0];
			fds[0].events = POLLIN;

			int child_alive = 1;
			char buf[BUFSIZE];

			while (child_alive)
			{
				// Check for command output with a short timeout (100ms)
				int poll_result = poll(fds, 1, 100);

				if (poll_result > 0)
				{
					if (fds[0].revents & POLLIN)
					{
						ssize_t bytes_read = read(output_pipe[0], buf, sizeof(buf) - 1);
						if (bytes_read > 0)
						{
							buf[bytes_read] = '\0';

							// Split output into lines and display each line
							char *line_start = buf;
							char *line_end;

							while ((line_end = strchr(line_start, '\n')) != NULL)
							{
								int line_len = line_end - line_start;
								if (line_len > 0)
								{
									char line[BUFSIZE];
									strncpy(line, line_start, line_len);
									line[line_len] = '\0';
									add_line_to_tab(tab, line);
								}
								line_start = line_end + 1;
							}

							// Handle any remaining data without newline
							if (line_start < buf + bytes_read)
							{
								int remaining_len = (buf + bytes_read) - line_start;
								if (remaining_len > 0)
								{
									char line[BUFSIZE];
									strncpy(line, line_start, remaining_len);
									line[remaining_len] = '\0';
									add_line_to_tab(tab, line);
								}
							}
							redraw();
						}
					}

					if (fds[0].revents & POLLHUP)
					{
						// Pipe closed, child might be done
						child_alive = 0;
					}
				}
				else if (poll_result < 0 && errno != EINTR)
				{
					// Poll error
					perror("poll");
					break;
				}

				// Check if child process is still alive (non-blocking wait)
				int status;
				pid_t result = waitpid(pid, &status, WNOHANG);
				if (result == pid)
				{
					// Child process finished
					child_alive = 0;

					// Read any remaining data from pipe
					while (1)
					{
						ssize_t bytes_read = read(output_pipe[0], buf, sizeof(buf) - 1);
						if (bytes_read > 0)
						{
							buf[bytes_read] = '\0';

							char *line_start = buf;
							char *line_end;
							while ((line_end = strchr(line_start, '\n')) != NULL)
							{
								int line_len = line_end - line_start;
								if (line_len > 0)
								{
									char line[BUFSIZE];
									strncpy(line, line_start, line_len);
									line[line_len] = '\0';
									add_line_to_tab(tab, line);
								}
								line_start = line_end + 1;
							}
							if (line_start < buf + bytes_read)
							{
								int remaining_len = (buf + bytes_read) - line_start;
								if (remaining_len > 0)
								{
									char line[BUFSIZE];
									strncpy(line, line_start, remaining_len);
									line[remaining_len] = '\0';
									add_line_to_tab(tab, line);
								}
							}
						}
						else
						{
							break;
						}
					}

					if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
					{
						char errbuf[256];
						snprintf(errbuf, sizeof(errbuf), "Command exited with status %d", WEXITSTATUS(status));
						add_line_to_tab(tab, errbuf);
					}
				}
				else if (result < 0)
				{
					// Error checking child status
					perror("waitpid");
					break;
				}

				// Process X11 events while waiting for command
				while (XPending(dpy) > 0)
				{
					XEvent ev;
					XNextEvent(dpy, &ev);

					if (ev.type == Expose)
					{
						redraw();
					}
					else if (ev.type == KeyPress)
					{
						char keybuf[32];
						KeySym ks;
						int len = XLookupString(&ev.xkey, keybuf, sizeof(keybuf), &ks, NULL);

						// Check for Ctrl+C or Ctrl+Z
						if (ev.xkey.state & ControlMask)
						{
							if (ks == XK_c)
							{
								kill(pid, SIGINT);
								add_line_to_tab(tab, "^C");
								redraw();
								child_alive = 0;
								break;
							}
							else if (ks == XK_z)
							{
								kill(pid, SIGTSTP);
								if (tab->suspended_count < 10)
								{
									tab->suspended_processes[tab->suspended_count].pid = pid;
									strncpy(tab->suspended_processes[tab->suspended_count].command,
											trimmed_cmd,
											sizeof(tab->suspended_processes[tab->suspended_count].command) - 1);
									tab->suspended_processes[tab->suspended_count].command[sizeof(tab->suspended_processes[tab->suspended_count].command) - 1] = '\0';
									tab->suspended_count++;
									char msg[256];
									snprintf(msg, sizeof(msg), "[%d] suspended", pid);
									add_line_to_tab(tab, msg);
								}
								add_line_to_tab(tab, "^Z");
								redraw();
								tab->child_pid = -1;
								child_alive = 0;
								break;
							}
						}
					}
				}

				redraw();
			}

			close(output_pipe[0]);
			tab->child_pid = -1;
		}
	}
	else
	{
		perror("fork");
		close(output_pipe[0]);
		close(output_pipe[1]);
	}
}

int main()
{
	setlocale(LC_ALL, "");

	// Initialize all tabs
	for (int i = 0; i < MAX_TABS; i++)
	{
		Tab *tab = &tabs[i];
		tab->in_pos = 0;
		tab->input_buf[0] = '\0';
		tab->line_count = 0;
		tab->shell_pid = 0;
		tab->scroll_offset = 0;
		tab->search_term[0] = '\0';
		tab->search_pos = 0;
		tab->in_search_mode = 0;
		tab->child_pid = -1;
		tab->auto_complete_count = 0;
		tab->bg_pid_count = 0;
		tab->suspended_count = 0;

		for (int j = 0; j < 10; j++)
		{
			tab->background_pids[j] = 0;
			tab->suspended_processes[j].pid = -1;
			tab->suspended_processes[j].command[0] = '\0';
		}

		// Initialize current directory for each tab
		if (getcwd(tab->current_directory, sizeof(tab->current_directory)) == NULL)
		{
			strcpy(tab->current_directory, "/");
		}

		snprintf(tab->tab_name, sizeof(tab->tab_name), "Tab %d", i + 1);
	}

	// Load history
	load_history();

	// Set up signal handlers
	struct sigaction sa_int, sa_tstp;
	sa_int.sa_handler = sigint_handler;
	sigemptyset(&sa_int.sa_mask);
	sa_int.sa_flags = SA_RESTART;
	sigaction(SIGINT, &sa_int, NULL);

	sa_tstp.sa_handler = sigtstp_handler;
	sigemptyset(&sa_tstp.sa_mask);
	sa_tstp.sa_flags = SA_RESTART;
	sigaction(SIGTSTP, &sa_tstp, NULL);

	dpy = XOpenDisplay(NULL);
	if (!dpy)
	{
		fprintf(stderr, "Cannot open display\n");
		exit(1);
	}

	int screen = DefaultScreen(dpy);
	win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, WIDTH, HEIGHT, 1,
							  BlackPixel(dpy, screen), BlackPixel(dpy, screen));

	XStoreName(dpy, win, "MyTerm");
	XSelectInput(dpy, win, ExposureMask | KeyPressMask);
	XMapWindow(dpy, win);

	font = XLoadQueryFont(dpy, "-*-fixed-medium-*-*-*-18-*-*-*-*-*-*-*");
	if (!font)
	{
		fprintf(stderr, "Cannot load fixed font\n");
		exit(1);
	}

	gc = XCreateGC(dpy, win, 0, NULL);
	XSetFont(dpy, gc, font->fid);
	XSetForeground(dpy, gc, WhitePixel(dpy, screen));
	XSetBackground(dpy, gc, BlackPixel(dpy, screen));

	colormap = DefaultColormap(dpy, screen);
	XAllocNamedColor(dpy, colormap, "green", &green_color, &green_color);
	XAllocNamedColor(dpy, colormap, "white", &white_color, &white_color);

	add_line_to_tab(&tabs[0], "Welcome to My Terminal");

	XEvent ev;
	while (1)
	{
		while (XPending(dpy) > 0)
		{
			XNextEvent(dpy, &ev);
			Tab *current_tab_ptr = &tabs[current_tab];

			switch (ev.type)
			{
			case Expose:
				redraw();
				break;

			case KeyPress:
			{
				char buf[32];
				KeySym ksym;
				XLookupString(&ev.xkey, buf, sizeof(buf), &ksym, NULL);

				if (current_tab_ptr->in_search_mode)
				{
					if (ksym == XK_Escape)
					{
						current_tab_ptr->in_search_mode = 0;
						current_tab_ptr->search_term[0] = '\0';
						current_tab_ptr->search_pos = 0;
					}
					else if (ksym == XK_Return)
					{
						search_in_history(current_tab_ptr->search_term, current_tab_ptr);
						current_tab_ptr->in_search_mode = 0;
						current_tab_ptr->search_term[0] = '\0';
						current_tab_ptr->search_pos = 0;
					}
					else if (ksym == XK_BackSpace)
					{
						if (current_tab_ptr->search_pos > 0)
						{
							current_tab_ptr->search_pos--;
							current_tab_ptr->search_term[current_tab_ptr->search_pos] = '\0';
						}
					}
					else if (buf[0] >= 32 && buf[0] <= 126)
					{
						if (current_tab_ptr->search_pos < BUFSIZE - 1)
						{
							current_tab_ptr->search_term[current_tab_ptr->search_pos] = buf[0];
							current_tab_ptr->search_pos++;
							current_tab_ptr->search_term[current_tab_ptr->search_pos] = '\0';
						}
					}
					redraw();
					break;
				}

				// Tab management shortcuts
				if (ev.xkey.state & ControlMask)
				{
					if (ksym == XK_t)
					{
						create_new_tab();
						redraw();
						break;
					}
					else if (ksym == XK_w)
					{
						close_current_tab();
						redraw();
						break;
					}
					else if (ksym == XK_Tab)
					{
						if (ev.xkey.state & ShiftMask)
						{
							switch_tab(-1);
						}
						else
						{
							switch_tab(1);
						}
						redraw();
						break;
					}
					else if (ksym == XK_r)
					{
						current_tab_ptr->in_search_mode = 1;
						current_tab_ptr->search_term[0] = '\0';
						current_tab_ptr->search_pos = 0;
						redraw();
						break;
					}
					else if (ksym == XK_l)
					{
						XClearWindow(dpy, win);
						redraw();
						break;
					}
					else if (ksym == XK_a)
					{
						move_cursor_start(current_tab_ptr);
						redraw();
						break;
					}
					else if (ksym == XK_e)
					{
						move_cursor_end(current_tab_ptr);
						redraw();
						break;
					}
				}

				// Handle regular input
				if (ksym == XK_Return)
				{
					if (current_tab_ptr->in_pos > 0)
					{
						// Checks if any unclosed quotes
						int open_quotes = 0;
						int open_single_quotes = 0;
						int in_escape = 0;

						for (int i = 0; i < current_tab_ptr->in_pos; i++)
						{
							if (in_escape)
							{
								in_escape = 0;
								continue;
							}

							if (current_tab_ptr->input_buf[i] == '\\')
							{
								in_escape = 1;
							}
							else if (current_tab_ptr->input_buf[i] == '"' && !open_single_quotes)
							{
								open_quotes = !open_quotes;
							}
							else if (current_tab_ptr->input_buf[i] == '\'' && !open_quotes)
							{
								open_single_quotes = !open_single_quotes;
							}
						}

						// If we have unclosed quotes, add a newline instead of executing
						if (open_quotes || open_single_quotes)
						{
							if (current_tab_ptr->in_pos < BUFSIZE - 2)
							{
								current_tab_ptr->input_buf[current_tab_ptr->in_pos] = '\n';
								current_tab_ptr->in_pos++;
								current_tab_ptr->input_buf[current_tab_ptr->in_pos] = '\0';
							}
						}
						else
						{
							// No unclosed quotes, execute the command
							current_tab_ptr->input_buf[current_tab_ptr->in_pos] = '\0';
							add_line_to_tab(current_tab_ptr, PROMPT);
							add_line_to_tab(current_tab_ptr, current_tab_ptr->input_buf);
							execute_command(current_tab_ptr, current_tab_ptr->input_buf);
							current_tab_ptr->in_pos = 0;
							current_tab_ptr->input_buf[0] = '\0';
						}
					}
				}
				else if (ksym == XK_BackSpace)
				{
					if (current_tab_ptr->in_pos > 0)
					{
						current_tab_ptr->in_pos--;
						current_tab_ptr->input_buf[current_tab_ptr->in_pos] = '\0';
					}
				}
				else if (ksym == XK_Delete)
				{
					// Delete is handled as backspace for simplicity
					if (current_tab_ptr->in_pos > 0)
					{
						current_tab_ptr->in_pos--;
						current_tab_ptr->input_buf[current_tab_ptr->in_pos] = '\0';
					}
				}
				else if (ksym == XK_Up)
				{
					current_tab_ptr->scroll_offset++;
					if (current_tab_ptr->scroll_offset > current_tab_ptr->line_count)
					{
						current_tab_ptr->scroll_offset = current_tab_ptr->line_count;
					}
				}
				else if (ksym == XK_Down)
				{
					current_tab_ptr->scroll_offset--;
					if (current_tab_ptr->scroll_offset < 0)
					{
						current_tab_ptr->scroll_offset = 0;
					}
				}
				else if (ksym == XK_Left)
				{
					if (current_tab_ptr->in_pos > 0)
					{
						current_tab_ptr->in_pos--;
					}
				}
				else if (ksym == XK_Right)
				{
					if (current_tab_ptr->in_pos < strlen(current_tab_ptr->input_buf))
					{
						current_tab_ptr->in_pos++;
					}
				}
				else if (ksym == XK_Tab)
				{
					handle_auto_complete(current_tab_ptr);
				}
				else if (buf[0] >= 32 && buf[0] <= 126)
				{
					if (current_tab_ptr->in_pos < BUFSIZE - 1)
					{
						current_tab_ptr->input_buf[current_tab_ptr->in_pos] = buf[0];
						current_tab_ptr->in_pos++;
						current_tab_ptr->input_buf[current_tab_ptr->in_pos] = '\0';
					}
				}
				redraw();
				break;
			}
			}
		}

		// Check for background processes in CURRENT TAB only
		Tab *current_tab_ptr = &tabs[current_tab];
		for (int i = 0; i < current_tab_ptr->bg_pid_count; i++)
		{
			if (current_tab_ptr->background_pids[i] > 0)
			{
				int status;
				pid_t result = waitpid(current_tab_ptr->background_pids[i], &status, WNOHANG);
				if (result > 0)
				{
					char msg[256];
					snprintf(msg, sizeof(msg), "[%d] done", current_tab_ptr->background_pids[i]);
					add_line_to_tab(current_tab_ptr, msg);
					current_tab_ptr->background_pids[i] = 0;
					redraw();
				}
			}
		}
		sleep(0.01); // 10ms
	}

	XCloseDisplay(dpy);
	return 0;
}