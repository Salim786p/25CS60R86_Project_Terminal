# My Terminal Project

## MyTerm

MyTerm is a feature-rich terminal emulator built with X11 that provides a modern commandline experience with tabs, history, auto-completion, and more.

## COMPILATION INSTRUCTIONS

1. Make sure you have X11 development libraries installed:
   - All the functionalities are tested and implemented in Ubuntu
   - On Ubuntu: `sudo apt-get install libx11-dev` will install X11 library on your computer

2. Compile the program:
   ```
   gcc MyTerm.c -o MyTerm -lX11
   ```

3. Run the terminal:
   ```
   ./MyTerm &
   ```

## FEATURES

- Multi-tab interface (Ctrl+T for new tab, Ctrl+W to close, Ctrl+Tab to switch)
- Command history with search (Ctrl+R)
- Auto-completion for filenames (Tab key)
- Input/output redirection (< and >)
- Pipe support for command chaining (|)
- MultiWatch for parallel command execution
- Signal handling (Ctrl+C, Ctrl+Z)
- Line navigation (Ctrl+A for start, Ctrl+E for end)
- Scrollable output with Up/Down arrows
- Persistent command history

## KEYBOARD SHORTCUTS

- Ctrl+T: Create new tab
- Ctrl+W: Close current tab  
- Ctrl+Tab: Switch among tabs
- Ctrl+R: Search command history
- Ctrl+C: Interrupt current command
- Ctrl+Z: Suspend current command
- Ctrl+A: Move cursor to start of line
- Ctrl+E: Move cursor to end of line
- Ctrl+L: Clear screen
- Tab: Auto-complete filenames
- Up/Down: Scroll through output

## USAGE EXAMPLES

Regular commands:
  ls -la
  pwd
  echo "Hello World"

Input redirection:
  sort < input.txt

Output redirection:
  ls -la > output.txt

Pipes:
  ls -la | grep ".c" | wc -l

MultiWatch (parallel execution):
  multiWatch [ "ls -la", "pwd", "whoami" ]

Other commands:
  cd directory    - Change directory
  history         - Show command history
  exit            - Close the terminal

## NOTES

- Command history is automatically saved to '.myterm_history.txt'
- Each tab maintains independent command history and state
- Use quotes for commands with spaces inside a squre bracket in MultiWatch
- The terminal supports multiline input