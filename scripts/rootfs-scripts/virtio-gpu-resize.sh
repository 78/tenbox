#!/bin/bash
# Auto-resize display when virtio-gpu hotplug event occurs

sleep 0.1
export DISPLAY=:0

# Try to find valid XAUTHORITY
OPENCLAW_UID=$(id -u openclaw 2>/dev/null || echo 1000)
for auth in /home/openclaw/.Xauthority /var/run/lightdm/openclaw/:0 /run/user/${OPENCLAW_UID}/gdm/Xauthority; do
    if [ -f "$auth" ]; then
        export XAUTHORITY="$auth"
        break
    fi
done

for output in Virtual-1 Virtual-0; do
    xrandr --output "$output" --auto 2>/dev/null && break
done
