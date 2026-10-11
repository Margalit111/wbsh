#!/bin/bash
# Process and system commands added to match busybox. Their output depends
# on the machine, so this script is checked by exit status only.

ps | head -1
pgrep -l wbsh > /dev/null && echo "pgrep finds wbsh"
pidof wbsh.exe > /dev/null && echo "pidof finds wbsh"

timeout 1 sleep 5
echo "builtin timed out: $?"
timeout 5 sleep 0.1
echo "fast command: $?"
timeout -s KILL 0.5 sleep 3
echo "killed: $?"

uptime > /dev/null && echo "uptime ok"
free -m | head -1
logname > /dev/null && echo "logname ok"
uuidgen | grep -cE '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$'
usleep 1000 && echo "usleep ok"
killall wbsh_no_such_process 2> /dev/null || echo "killall reports nothing killed"
