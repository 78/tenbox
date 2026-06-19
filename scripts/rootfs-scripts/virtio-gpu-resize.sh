#!/bin/bash
# Auto-resize display when virtio-gpu hotplug event occurs

sleep 0.1
export DISPLAY=:0

# Try to find valid XAUTHORITY
AGENTSPHERE_UID=$(id -u admin 2>/dev/null || echo 1000)
for auth in /home/admin/.Xauthority /var/run/lightdm/admin/:0 /run/user/${AGENTSPHERE_UID}/gdm/Xauthority; do
    if [ -f "$auth" ]; then
        export XAUTHORITY="$auth"
        break
    fi
done

for output in Virtual-1 Virtual-0; do
    xrandr --output "$output" --auto 2>/dev/null && break
done
