#!/bin/sh
set -eu

project_dir=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
make -C "$project_dir" tcp_server tcp_server_reverse tcp_server_forward

exec osascript - "$project_dir" <<'APPLESCRIPT'
on run argv
    set projectDir to item 1 of argv
    set launchPrefix to "cd " & quoted form of projectDir & " && ulimit -Sn 32768 && "
    set servers to {¬
        {"원 서버 9000", "./build/tcp_server -p 9000 ./public"}, ¬
        {"원 서버 9001", "./build/tcp_server -p 9001 ./public"}, ¬
        {"리버스 프록시 9200", "./build/tcp_server_reverse -p 9200 -b 9000 -b 9001 -H 5 ./public"}, ¬
        {"포워드 프록시 9100", "./build/tcp_server_forward -p 9100 -o 9200"}}

    tell application "/System/Applications/Utilities/Terminal.app"
        repeat with serverConfig in servers
            set serverTab to do script (launchPrefix & item 2 of serverConfig)
            set custom title of serverTab to item 1 of serverConfig
            set title displays custom title of serverTab to true
        end repeat
        activate
    end tell
end run
APPLESCRIPT
