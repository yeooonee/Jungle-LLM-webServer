#!/usr/bin/env bash
set -eo pipefail

cd "$(dirname "$0")/.."
demo_dir=$(mktemp -d "${TMPDIR:-/tmp}/jungle-demo.XXXXXX")
server_pids=()

cleanup() {
    for pid in "${server_pids[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${server_pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    rm -rf -- "$demo_dir"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

start_server() {
    local program=$1 port=$2 pid log
    shift 2
    log="$demo_dir/$program-$port.log"
    if nc -z 127.0.0.1 "$port" 2>/dev/null; then
        printf '포트 %s에서 이미 서버가 실행 중입니다. 해당 서버를 종료한 뒤 데모를 실행하세요.\n' "$port" >&2
        return 1
    fi

    "$demo_dir/build/$program" -p "$port" "$@" >"$log" 2>&1 &
    pid=$!
    server_pids+=("$pid")
    for attempt in {1..100}; do
        if ! kill -0 "$pid" 2>/dev/null; then
            break
        fi
        if nc -z 127.0.0.1 "$port" 2>/dev/null; then
            return 0
        fi
        sleep 0.03
    done

    printf '%s의 시작을 확인하지 못했습니다 (포트 %s).\n' "$program" "$port" >&2
    cat "$log" >&2
    return 1
}

show_step() {
    printf '\n%s\nEnter로 실행 ' "$1"
    IFS= read -r
    printf '\n'
}

request_through_chain() {
    local path=$1
    shift
    curl -fsS --max-time 8 --noproxy '' --proxy "http://127.0.0.1:$forward_port" \
        "$@" "http://127.0.0.1:$reverse_port$path"
}

header_value() {
    awk -v name="$1:" 'tolower($1) == tolower(name) {
        sub(/^[^:]*:[ \t]*/, ""); sub(/\r$/, ""); print; exit
    }' "$2"
}

read_backend_port() {
    local port
    port=$(header_value X-Backend-Port "$1")
    if [[ -z $port ]]; then
        printf '응답에서 X-Backend-Port를 찾지 못했습니다.\n' >&2
        cat "$1" >&2
        return 1
    fi
    printf '%s' "$port"
}

show_http_exchange() {
    local path=$1 backend_port request_line status
    local trace="$demo_dir/request.trace"
    local headers="$demo_dir/response.headers"
    local body="$demo_dir/response.body"

    if ! request_through_chain "$path" --http1.1 --verbose \
        -H 'Cache-Control: no-cache' -D "$headers" -o "$body" 2>"$trace"; then
        cat "$trace" >&2
        return 1
    fi

    backend_port=$(read_backend_port "$headers")
    request_line=$(tr -d '\r' < "$trace" | sed -n 's/^> GET /GET /p')
    status=$(head -n 1 "$headers" | tr -d '\r')

    printf '요청 →  %s\n' "$request_line"
    printf '        curl → 포워드 %s → 리버스 %s → 원 서버 %s\n' \
        "$forward_port" "$reverse_port" "$backend_port"
    printf '중계    GET %s HTTP/1.1\n' "$path"
    printf '응답 ←  %s · %s\n' "$status" "$(header_value Content-Type "$headers")"
    printf '        %s\n' "$(head -n 1 "$body")"
}

# Prefer the README ports. Use free loopback ports when another server is running.
read -r html_port cgi_port reverse_port forward_port cache_port < <(python3 - <<'PY'
from contextlib import ExitStack
from errno import EADDRINUSE
import socket

with ExitStack() as reservations:
    ports = []
    for preferred in (9000, 9001, 9200, 9100, 9101):
        listener = reservations.enter_context(socket.socket())
        try:
            listener.bind(("127.0.0.1", preferred))
        except OSError as error:
            if error.errno != EADDRINUSE:
                raise
            listener.bind(("127.0.0.1", 0))
        ports.append(listener.getsockname()[1])
    print(*ports)
PY
)

printf '데모 서버 시작 중…\n'
make -s "BUILD_DIR=$demo_dir/build" tcp_server tcp_server_forward tcp_server_reverse
unset JUNGLE_ORIGIN_TOKEN
start_server tcp_server "$html_port" ./public
start_server tcp_server "$cgi_port" ./public
start_server tcp_server_reverse "$reverse_port" -b "$html_port" -b "$cgi_port" \
    -r "/static/index.html=$html_port" -r "/cgi-bin/sum=$cgi_port" ./public
start_server tcp_server_forward "$forward_port" -o "$reverse_port"

show_step '1. HTML · CGI'
show_http_exchange /static/index.html
printf '\n'
show_http_exchange '/cgi-bin/sum?a=3&b=5'

show_step '2. Round-Robin'
headers="$demo_dir/response.headers"
selected_ports=()
for request in 1 2 3; do
    request_through_chain / -D "$headers" -o /dev/null
    backend_port=$(read_backend_port "$headers")
    selected_ports+=("$backend_port")
done
printf 'GET / 세 번 → 원 서버 %s → %s → %s\n' "${selected_ports[@]}"

show_step '3. 포워드 캐시'
start_server tcp_server_forward "$cache_port" -o "$html_port"
printf 'GET /static/index.html 세 번 · 포워드 %s\n' "$cache_port"
for request in 1 2 3; do
    curl -fsS --max-time 8 --noproxy '' --proxy "http://127.0.0.1:$cache_port" \
        -D "$headers" -o /dev/null "http://127.0.0.1:$html_port/static/index.html"
    age=$(header_value Age "$headers")
    if [[ -n $age ]]; then
        printf '%s번째  포워드 캐시 · Age: %s\n' "$request" "$age"
    else
        printf '%s번째  원 서버 %s\n' "$request" "$html_port"
    fi
done
