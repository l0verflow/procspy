#include <ncurses.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <pwd.h>
#include <fcntl.h>
#include <time.h>

#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))
#define MAX_PROCESSES 1024

typedef struct {
    int pid;
    char user[32];
    char command[256];
} process_info;

process_info processes[MAX_PROCESSES];
int process_count = 0;
int current_selection = 0;
int top_index = 0;

void
colors() 
{
  start_color();
  init_pair(1, COLOR_RED, COLOR_BLACK);
  init_pair(2, COLOR_GREEN, COLOR_BLACK);
  init_pair(3, COLOR_YELLOW, COLOR_BLACK);
  init_pair(4, COLOR_BLUE, COLOR_BLACK);
  init_pair(5, COLOR_MAGENTA, COLOR_BLACK);
  init_pair(6, COLOR_CYAN, COLOR_BLACK);
  init_pair(7, COLOR_WHITE, COLOR_BLACK);
}

void
get_process_info (int pid, 
                  char *user, 
                  char *command)
{
  char path[256], buffer[256];
  FILE *fp;
  struct passwd *pw;
  uid_t uid;

  snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
  fp = fopen(path, "r");
  if (fp) 
    {
      size_t size = fread(buffer, sizeof(char), sizeof(buffer), fp);
      fclose(fp);
      if (size > 0) 
        {
          buffer[size] = '\0';
          for (size_t i = 0; i < size; i++) 
            {
              if (buffer[i] == '\0')
                {
                  buffer[i] = ' ';
                }
            }
          strncpy(command, buffer, 256);
        }
      else 
        {
          strcpy(command, "N/A");
        }
    } 
  else
    {
      strcpy(command, "N/A");
    }

  snprintf(path, sizeof(path), "/proc/%d/status", pid);
  fp = fopen(path, "r");
  if (fp) 
    {
      while (fgets(buffer, sizeof(buffer), fp)) 
        {
          if (strncmp(buffer, "Uid:", 4) == 0)
            {
              sscanf(buffer, "Uid:\t%d", &uid);
              pw = getpwuid(uid);
              if (pw)
                {
                  strncpy(user, pw->pw_name, 32);
                }
              else
                {
                  strcpy(user, "Unknown");
                }
              break;
            }
        }
      fclose(fp);
    } 
  else
    {
      strcpy(user, "Unknown");
    }
}

void
refresh_p ()
{
  DIR *dp;
  struct dirent *entry;
  int index = 0;

  dp = opendir("/proc");
  if (dp == NULL)
    {
      perror("opendir");
      return;
    }

  while ((entry = readdir(dp)) != NULL)
    {
      if (entry->d_type == DT_DIR && atoi(entry->d_name) > 0)
        {
          processes[index].pid = atoi(entry->d_name);
          get_process_info(processes[index].pid, processes[index].user, processes[index].command);
          index++;
          if (index >= MAX_PROCESSES) break;
        }
    }

  closedir(dp);
  process_count = index;
}

void
display (WINDOW *win)
{
  int y = 1;
  char timestamp[32];
  time_t now = time(NULL);
  struct tm *t = localtime(&now);

  strftime(timestamp, sizeof(timestamp)-1, "%Y/%m/%d %H:%M:%S", t);

  for (int i = top_index; i < process_count && y < LINES - 1; i++)
    {
      if (i == current_selection)
        {
          wattron(win, A_REVERSE);
        }

      mvwprintw(win, y++, 1, "%s CMD: %-10s UID: %-10s PID: %-10d %s", timestamp, processes[i].command, processes[i].user, processes[i].pid, processes[i].command);

      if (i == current_selection)
        {
          wattroff(win, A_REVERSE);
        }
    }
}

void
handle_e (int fd, WINDOW *win)
{
  char buffer[EVENT_BUF_LEN];
  int length, i = 0;

  while (1)
    {
      length = read(fd, buffer, EVENT_BUF_LEN);
      if (length < 0)
        {
          perror("read");
        }

      i = 0;
      while (i < length)
        {
          struct inotify_event *event = (struct inotify_event *) &buffer[i];
          if (event->len)
            {
              if (event->mask & IN_ACCESS || event->mask & IN_MODIFY)
                {
                  refresh_p();
                  wclear(win);
                  box(win, 0, 0);
                  mvwprintw(win, 0, 1, "Process Monitor");
                  display(win);
                  refresh_p(win);
                }
            }
          i += EVENT_SIZE + event->len;
        }
    }
}

void
cleanup (int signum)
{
  endwin();
  exit(0);
}

void
handle_i (WINDOW *win)
{
  int ch;

  while (1)
    {
      ch = wgetch(win);
      switch (ch)
        {
          case KEY_UP:
            if (current_selection > 0) current_selection--;
            if (current_selection < top_index) top_index = current_selection;
            break;
          case KEY_DOWN:
            if (current_selection < process_count - 1) current_selection++;
            if (current_selection >= top_index + LINES - 2) top_index++;
            break;
          case 'q':
            cleanup(0);
            break;
        }

      wclear(win);
      box(win, 0, 0);
      mvwprintw(win, 0, 1, "Process Monitor");
      display(win);
      refresh_p(win);
    }
}

void
update (WINDOW *win)
{
  while (1)
    {
      refresh_p();
      wclear(win);
      box(win, 0, 0);
      mvwprintw(win, 0, 1, "Process Monitor");
      display(win);
      refresh_p(win);
      sleep(2);
    }
}

int
main ()
{
  int fd, wd;
  initscr();
  cbreak();
  noecho();
  curs_set(FALSE);

colors();

  WINDOW *win = newwin(LINES, COLS, 0, 0);
  keypad(win, TRUE);
  box(win, 0, 0);
  mvwprintw(win, 0, 1, "Process Monitor");
  refresh_p(win);

  fd = inotify_init();
  if (fd < 0)
    {
      perror("inotify_init");
      return 1;
    }

  wd = inotify_add_watch(fd, "/tmp", IN_ACCESS | IN_MODIFY);
  if (wd == -1)
    {
      perror("inotify_add_watch");
      return 1;
    }

  refresh_p();
  display(win);
  refresh_p(win);

  signal(SIGINT, cleanup);

  if (fork() == 0) 
    {
      handle_i(win);
    } 
  else if (fork() == 0) 
    {
      update(win);
    }
  else 
    {
      handle_e(fd, win);
    }

  inotify_rm_watch(fd, wd);
  close(fd);
  endwin();
  return 0;
}
