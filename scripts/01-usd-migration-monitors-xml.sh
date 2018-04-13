#!/bin/bash

config_path="$HOME/.config"

if [ -n "$XDG_CONFIG_HOME" ]; then
    config_path="$XDG_CONFIG_HOME";
fi

for monitor_file in monitors.xml monitors.xml.backup; do
    old_monitors="$config_path/$monitor_file"
    new_monitors="$config_path/unity-$monitor_file"

    if [ -f "$old_monitors" ]; then
        if grep '<monitors[^>]*>' "$old_monitors" | grep -Fq 'version="1"'; then
           mv -v "$old_monitors" "$new_monitors"
        fi
    fi
done
