# MyTerm — Design Documentation

---

## Task 1: Graphical User Interface (X11)

### Implementation
- **X11 functions used:** `XOpenDisplay()`, `XCreateSimpleWindow()`, `XMapWindow()`, `XSelectInput()`, `XDrawString()`
- Text buffer system built using `Line` struct array.
- Each `Tab` maintains independent state:
  - Input buffer
  - Display lines
  - Scroll position
  - Search state
  - Child process ID
  - current_directory
- This Ensures each tab has an independent working directory state, preventing state leakage between tabs.
- Tab headers displayed at the top with current tab highlighted.
- **Keyboard Shortcuts:**
  - `Ctrl + T` → New tab
  - `Ctrl + W` → Close tab
  - `Ctrl + Tab` → Switch tabs

---

## Task 2: Run External Commands

### Implementation
- `fork()` → creates child process for command execution.
- All external commands are executed using **`execvp()`** or **`execl("/bin/bash", "bash", "-c", command)`**
- `pipe()` → captures command output for GUI display.
- Parent reads from the pipe line-by-line and displays it.
- `waitpid()` ensures completion before next input.

---

## Task 3: Multiline Unicode Input

### Implementation
- Enabled Unicode via `setlocale(LC_ALL, "")`.
- The **`Return`** key handler checks for unbalanced quotes (single **`'`** and double **`"`**) and backslash escapes (Line 1461). If quotes are unbalanced, a newline character (`\n`) is inserted instead of executing the command, correctly implementing multiline input.
- `Enter` inserts newline instead of executing when unbalanced.
- Proper rendering of multiline input with `\n`.
- *Currently supports English only.*



**Example:**
```bash
echo "Hello
World"
```

---

## Task 4: Input Redirection (`<`)

### Implementation
- Command parser detects `<` symbol.
- Extracts filename following `<`.
- In child process (before exec):
  ```c
  fd_in = open(filename, O_RDONLY);
  dup2(fd_in, STDIN_FILENO);
  close(fd_in);
  ```

**Examples:**
```bash
./a.out < input.txt
sort < data.txt
```

---

## Task 5: Output Redirection (`>`)

### Implementation
- Detects `>` symbol in command string.
- Extracts filename after `>`.
- In child process:
  ```c
  fd_out = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  dup2(fd_out, STDOUT_FILENO);
  dup2(fd_out, STDERR_FILENO);
  close(fd_out);
  ```

**Combined Redirection Example:**
```bash
./a.out < input.txt > output.txt
ls > filelist.txt
```

---

## Task 6: Pipe Support (`|`)

### Implementation
- Parser splits input by `|`.
- For N commands → creates N-1 pipes.
- Forks N child processes:
  - First → stdin = `/dev/null`
  - Middle → stdin = prev pipe, stdout = next pipe
  - Last → stdout = GUI pipe
- Uses `dup2()` for pipe redirection.
- Parent waits for all child processes.
- **Output Termination:** The pipe logic ensures the write-end of the final **`gui_pipe`** is closed in the parent and all child processes. This guarantees the parent's **`read()`** correctly receives an **EOF** (returns 0) when the entire pipeline is complete.

**Examples:**
```bash
ls *.txt | wc -l
cat file.txt | sort | uniq
```

---

## Task 7: Multiwatch Command

**Command Format:**
```bash
multiWatch ["cmd1", "cmd2", "cmd3", ...]
```

### Execution
1. Parses list using `parse_multiwatch_list()`.
2. Forks separate process for each command.
3. Each child:
   - Creates temp file `.temp.PID.txt`.
   - Redirects output to file using `dup2()`.
   - Executes via `execl("/bin/bash", "bash", "-c", command)`.
4. **File Monitoring:** Each child process redirects its output to a unique temporary file in **/tmp** (e.g., **/tmp/.temp_mw_PID.txt**). The parent process uses `poll()` to monitor for file size changes and `lseek()` to read only the newly appended content.
5. Displays updates with timestamps and command name.


**Output Example:**
```
"cmd1", 2024-01-15 14:30:25
----------------------------------------------------
Output line 1
Output line 2
----------------------------------------------------
```

**Signal Handling:**
- `Ctrl + C` stops all children (SIGKILL) and removes temp files.

---

## Task 8: Line Navigation (Ctrl + A / E)

### Implementation
- Detects Ctrl+A (`0x01`) and Ctrl+E (`0x05`).
- Moves cursor to start or end respectively.
- `in_pos` variable tracks cursor position.
- Cursor visually shown as underline rectangle.
- Left/right arrow keys supported for navigation.

---

## Task 9: Signal Handling

### SIGINT (Ctrl + C)
- Registered via `sigaction()`.
- If `child_pid > 0`, sends `SIGINT`.
- Displays "^C" and regains shell control.

### SIGTSTP (Ctrl + Z)
- Sends `SIGTSTP` to active child.
- Adds to `background_pids[]`.
- **Ctrl+Z** places the process in the **`suspended_processes`** array, which stores the PID and the original command string. The **`fg`** built-in sends **`SIGCONT`** to the process and calls a blocking **`waitpid`** to transition it back to the foreground.
- During foreground command execution, the parent uses poll() with a 100ms timeout on the output pipe, and checks for XPending() events.
-Prevents the GUI from freezing while waiting for command output, ensuring the terminal remains responsive to **Ctrl+C** and **Ctrl+Z**.
---

## Task 10: Searchable Shell History

### Storage
- Stored in `.myterm_history.txt` (persistent across sessions).

### Features
- `history` command → lists last 1000 entries.
- Pagination handled by `show_history_from_file()`.

### Search (Ctrl + R)
- Prompts `Search: `.
- Finds most recent match (exact/partial).
- Displays multiple matches with numbering.
- Selected result replaces input buffer.

---

## Task 11: Auto-Complete for Filenames

### Implementation
- Triggered by `Tab` key → `handle_auto_complete()`.
- Extracts current word → scans directory.
- **Single match:** completes automatically.  
- **Multiple matches:** When multiple matches are found, the system uses `stat()` to check if a match is a directory and appends a trailing slash (**`/`**) to the displayed name for better user feedback.

**Example:**
```bash
Directory: [abc.txt, abcd.txt, def.txt]

Input: ls ab + Tab → ls abc
Input: ls abc + Tab → 1. abc.txt  2. abcd.txt
```

---

## Summary

| Feature | Functions/System Call |
|:--------|:----------------------|
| GUI | X11 (XOpenDisplay, XDrawString) |
| Command Execution | fork(), execvp(), pipe() |
| Redirection | dup2(), open() |
| Pipes | pipe(), fork(), dup2() |
| Multiwatch | poll(), execl() |
| Signals | sigaction(), kill() |
| History | File I/O |
| Auto-complete | Directory scanning |

---
---