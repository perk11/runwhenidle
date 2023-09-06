# runwhenidle

runwhenidle is a Linux utility that can be used to run a computationally or IO-intensive program when user is not
in front of the computer, pausing it once the user is back, resuming once the user left, often without requiring adaptation from the program being run.


runwhenidle runs a command given to it, pauses it if the user is active by sending SIGTSTP (or optionally SIGSTOP) to the command, 
when the user activity stops, runwhenidle resumes the command by sending it SIGCONT signal.
It then checks once per second if user activity has resumed, and once it is, pauses the command again.

runwhenidle uses XScreenSaverQueryInfo() to check when last user activity happened therefore a running X server is required.
Wayland is not currently supported.

If runwhenidle receives an interruption signal (SIGINT or SIGTERM), it will pass that signal to the command it is
running, resume the command if it previously paused it, stop checking for user activity and will wait for the command
to handle the signal.

## Installation

**Ubuntu and Debian**: Download the deb file attached to the [latest release](https://github.com/perk11/runwhenidle/releases/latest).
**Arch**: Available in AUR: https://aur.archlinux.org/packages/runwhenidle

Other Distros: You will need to compile runwhenidle yourself.

## Compiling

Make sure you have `gcc`, `make`, `git` and `libxss-dev` installed. Run `make release`. This should produce a binary file `runwhenidle` in the project directory.

If you want to install it system-wide, run `sudo make install` or simply `sudo cp ./runwhenidle /usr/bin`. 

## Usage

    runwhenidle [OPTIONS] shell_command_to_run [shell_command_arguments]

### Options

| Option                            | Description                                                                                                                                                | Default Value |
|-----------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------|
| `--timeout\| -t <seconds>`        | Set the user idle time after which the command can run in seconds.                                                                                         | 300 seconds   |
| `--start-monitor-after\| -a <ms>` | Set an initial delay in milliseconds before monitoring starts. During this time command runs unrestricted. This helps to catch quick errors.               | 300 ms        |
| `--pause-method\| -m <method>`    | Specify method for pausing the command when the user is not idle. Available Options: SIGTSTP (can be ignored by the program), SIGSTOP (cannot be ignored). | SIGTSTP       |
| `--verbose\| -v`                  | Enable verbose output for monitoring.                                                                                                                      | Not verbose   |
| `--debug`                         | Enable debugging output.                                                                                                                                   | No debug      |
| `--quiet\| -q`                    | Suppress all output from ./runwhenidle except errors and only display output from the command that is running.                                             | Not quiet     |
| `--version\| -V`                  | Print the program version information.                                                                                                                     |               |

### Example 1:
    
    runwhenidle -t 100 -v cp /filea /fileb

Run the `cp` command and pause it while user is active. When user is inactive for 100 seconds, resume the command.
Output debug information to stderr.

### Example 2:

    runwhenidle --timeout=300 -q cat /dev/zero

Run the `cat /dev/zero` command and pause it while user is active. `-q` option makes sure runwhenidle doesn't output anything other than the output of `cat /dev/zero`. 


### Building Ubuntu/Debian package:
Make sure you have docker installed and run:

    make debian-package

The .deb file will be generated in `package-build/` directory.
