# runwhenidle

runwhenidle is a Linux utility that can be used to pause a computationally or IO-intensive program when user is
in front of the computer, resuming it once the user is away, usually without requiring adaptation from the program 
being run. It can run a command given to it or monitor an already running process.

## Example 1:

The simplest way to start using runwhenidle is just by putting it in front of the command:

    runwhenidle rsync -rva /home /backup

## Example 2:

    runwhenidle --start-monitor-after=10 --timeout=30 --quiet ffmpeg -i file.mp4 file.mkv

In this example `ffmpeg` will run for 10 seconds uninterrupted. After 10 seconds pass, 
runwhenidle  will start monitoring user activity and pause ffmpeg  if user has been active within last 30 seconds.
If user is inactive for 30 seconds, resume the operation of ffmpeg. `--quiet` option makes sure runwhenidle doesn't
output anything other than the output of `ffmpeg`. Same command can be ran with the short versions of the arguments:

    runwhenidle -a 10000 -t 30 -q ffmpeg -i file.mp4 file.mkv


## Example 3:

    runwhenidle --pid=12345 --pause-method=SIGTSTP --verbose

Start monitoring user activity and when user is active, pause process with PID 12345. Unpause it when user is inactive 
for 300 seconds. Use SIGTSTP which the process 12345 can handle or ignore instead of the default SIGSTOP that is
always handled by the OS.

## Under the Hood: How runwhenidle Controls Processes Based on User Activity

runwhenidle uses XScreenSaverQueryInfo() to check when last user activity happened.
Therefore, running X server is required. Wayland is not currently supported.
When user is active, these checks happen as infrequently as possible to satisfy the desired inactivity timeout 
(default - every 5 minutes). When user is inactive, these checks happen once per second, to allow to restore
the system responsiveness quickly.

When runwhenidle determines that the user is active, it will send SIGSTOP (or optionally SIGTSTP) to the process
and all its child processes. When the user activity stops, runwhenidle resumes the process by sending it and all 
the child processes SIGCONT signal. It then checks once per second if user activity has resumed, and once it is,
pauses the process and its child processes again.

If runwhenidle was used to run a command (i.e. `--pid` parameter was not used) and it receives an interruption
signal (SIGINT or SIGTERM), it will resume the process it is running if it is currently paused, and then sned the
same signal to it to allow the process to handle the signal. runwhenidle then will stop checking for user activity 
and will wait for the process to exit.

## Installation

**Ubuntu and Debian**: Download the deb file attached to
the [latest release](https://github.com/perk11/runwhenidle/releases/latest).

**Arch**: Available in AUR: https://aur.archlinux.org/packages/runwhenidle

Other Distros: You will need to compile runwhenidle yourself.

## Compiling

Make sure you have `gcc`, `make`, `git` and `libxss-dev` installed. Run `make release`. This should produce a binary
file `runwhenidle` in the project directory.

If you want to install it system-wide, run `sudo make install` or simply `sudo cp ./runwhenidle /usr/bin`.

## Usage

    runwhenidle [OPTIONS] [shell_command_to_run] [shell_command_arguments]

### Options

| Option                           | Description                                                                                                                                                | Default Value |
|----------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------|
| `--timeout, -t <seconds>`        | Set the user idle time after which the process can be resumed in seconds.                                                                                  | 300 seconds   |
| `--pid, -p <pid>`                | Monitor an existing process. When this option is used, shell_command_to_run should not be passed.                                                          |               |
| `--start-monitor-after, -a <ms>` | Set an initial delay in milliseconds before monitoring starts. During this time the process runs unrestricted. This helps to catch quick errors.           | 300 ms        |
| `--pause-method, -m <method>`    | Specify method for pausing the process when the user is not idle. Available Options: SIGTSTP (can be ignored by the program), SIGSTOP (cannot be ignored). | SIGSTOP       |
| `--quiet, -q`                    | Suppress all output from ./runwhenidle except errors and only display output from the command that is running. No output if `--pid` options is used.       | Not quiet     |
| `--verbose, -v`                  | Enable verbose output for monitoring.                                                                                                                      | Not verbose   |
| `--debug`                        | Enable debugging output.                                                                                                                                   | No debug      |
| `--version,  -V`                 | Print the program version information.                                                                                                                     |               |


### Known issues

1. Wayland support. runwhenidle currently doesn't work without XScreenSaver, but Wayland support should be possible and
   is planned (at least for the DEs supporting ext-idle-notify, which now both Gnome and KDE support).
2. When monitoring an existing pid, once it gets paused, it gets detached from the terminal it was in.
   Running "fg" command could be a workaround to get it reattached, but it is required after every pause.

### Building Ubuntu/Debian package

Make sure you have docker installed and run:

    make debian-package

The .deb file will be generated in `package-build/` directory.
