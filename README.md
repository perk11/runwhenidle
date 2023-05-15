runwhenidle is a Linux utility that can be used to run a computationally or IO-intensive program when user is not
in front of the computer, similar to a screensaver, often without requiring adaptation from the program being ran.


runwhenidle runs a command given to it, pauses it if the user is active by sending SIGTSTP to the command, 
when the user activity stops, runwhenidle resumes the command by sending it SIGCONT signal.
It then checks once per second if user activity has resumed, and once it is, pauses the command again.

runwhenidle uses XScreenSaverQueryInfo() to check when last user activity happened therefore a running X server is required.
Wayland is not currently supported.

Usage:

    runwhenidle [--timeout|-t timeout_value_in_seconds] [--verbose|-v] [--quiet|-q] shell_command_to_run

`--timeout` or `-t` specifies how many seconds of user inactivity are enough to resume the command. Default value is 300/5 minutes.

`--verbose` or `-v` adds additional debug output
`--quiet` or `-q` suppresses all the output from `runwhenidle` and only displays output from the command that is running.  

### Example 1:
    
    runwhenidle -t 100 -v "cp /filea /fileb"

Run the `cp` command and pause it while user is active. When user is inactive for 100 seconds, resume the command.
Output debug information to stderr.

### Example 2:

    runwhenidle --timeout=300 -q "cat /dev/zero"

Run the `cat /dev/zero` command and pause it while user is active. `-q` option makes sure runwhenidle doesn't output anything other than the output of `cat /dev/zero`. 