launch-daemon
=============
This is a simple ANSI C Linux program that daemonizes naughty programs.  I have found
that many scripts and web languages do not properly daemonize, and instead hang onto
controlling terminals or files.  This can cause them to terminate strangely when you
exit the controlling shell.

This fixes that by redirecting I/O and terminals appropriately, as well as setting
the controlling process group to the scheduler.  By default, output is redirected to /dev/null.
When launch-daemon successfully launches the new process, it will print the new PID to
stdout.

Compilation
-----------
Type:

    make

Yay!

Usage
-----
Standard usage is:

    launch-daemon [options] program [arguments ...]

Simple example:

    launch-daemon -s log/resque_output.log rake start_resque_workers >resque.pid

The redirects any output (either stdout or stderr) via -s, and saves the PID into the
resque.pid file.  

Options
-------

    -h                    # display usage
    -v                    # print version
    -s stderrfile         # redirect output to file (default: /dev/null)
    -p port               # bind and listen on the specified port (not normally needed)
    -c                    # run command ala sh -c; detach but do not daemonize
    -R /chroot            # chroot to the specified path before forking (security)
    -L /ld/lib/path       # set LD_LIBRARY_PATH for programs that need it
    -E exename            # set the program name (overrides shell)
    -u user               # switch to the specified user before forking
    --                    # end of argument parsing

