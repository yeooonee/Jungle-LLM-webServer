-- 검증 콜백의 비용을 포함한 HTTP/1.1 프록시 부하를 측정한다.
local threads = {}
local payload = string.rep("x", 1024)
seen = 0
valid = 0
invalid = 0
non_200 = 0

function setup(thread)
  table.insert(threads, thread)
end

function init(args)
  wire = "GET /api/fixture.txt HTTP/1.1\r\nHost: " .. wrk.host .. ":" .. wrk.port ..
         "\r\nConnection: close\r\n\r\n"
end

function request()
  return wire
end

function response(status, headers, body)
  seen = seen + 1
  if status ~= 200 then non_200 = non_200 + 1 end
  local length, connection
  for name, value in pairs(headers) do
    if string.lower(name) == "content-length" then length = value end
    if string.lower(name) == "connection" then connection = string.lower(value) end
  end
  if status == 200 and body == payload and length == "1024" and connection == "close" then
    valid = valid + 1
  else
    invalid = invalid + 1
  end
end

function done(summary, latency, requests)
  local observed, correct, incorrect, statuses = 0, 0, 0, 0
  for _, thread in ipairs(threads) do
    observed = observed + thread:get("seen")
    correct = correct + thread:get("valid")
    incorrect = incorrect + thread:get("invalid")
    statuses = statuses + thread:get("non_200")
  end
  local e = summary.errors
  io.write(string.format(
    '\nCHECK_JSON {"completed":%.0f,"observed":%.0f,"valid":%.0f,"invalid":%.0f,' ..
    '"non_200":%.0f,"duration_us":%.0f,"bytes":%.0f,' ..
    '"connect_errors":%.0f,"read_errors":%.0f,"write_errors":%.0f,"timeouts":%.0f,' ..
    '"status_errors":%.0f,"latency_mean_us":%.3f,"latency_p95_us":%.3f}\n',
    summary.requests, observed, correct, incorrect, statuses, summary.duration, summary.bytes,
    e.connect, e.read, e.write, e.timeout, e.status, latency.mean, latency:percentile(95)))
end
