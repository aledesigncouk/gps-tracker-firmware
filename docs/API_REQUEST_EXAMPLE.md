# Example API request

This is the exact raw HTTP request the tracker sends for each GPS fix, built
in `postFixToServer()` in [`src/main.cpp`](../src/main.cpp). It's sent as
plain text over a raw TCP socket (`sim808.send()`), not through any HTTP
library - the SIM808 module has no reliable HTTPS support, so this only
works because the backend accepts plain HTTP on port 80 for this endpoint.

## Template

```
POST /nautilus/api/point?lat=<lat>&lon=<lon>&timestamp=<timestamp> HTTP/1.1
Host: www.yoroxid.com
X-API-KEY: <API_KEY>
Content-Length: 0

```

- `<lat>` / `<lon>` - decimal degrees, 6 decimal places (from `dtostrf(fix.lat, 1, 6, ...)`)
- `<timestamp>` - ISO 8601 UTC, built from the GPS fix's own UTC time (`AT+CGNSINF`), not the Arduino's clock
- `<API_KEY>` - the value of `API_KEY` in `include/secrets.h`
- No body - everything needed is in the query string, so `Content-Length: 0`
- Request ends with a blank line (`\r\n\r\n`) same as any HTTP request with no body

## Real example

Captured during testing, using an actual GPS fix:

```
POST /nautilus/api/point?lat=52.660950&lon=-2.482392&timestamp=2026-08-31T01:22:23Z HTTP/1.1
Host: www.yoroxid.com
X-API-KEY: <API_KEY>
Content-Length: 0

```

## Equivalent curl command

For testing the backend directly from a computer, without the Arduino:

```
curl -X POST "http://www.yoroxid.com/nautilus/api/point?lat=52.660950&lon=-2.482392&timestamp=2026-08-31T01:22:23Z" \
     -H "X-API-KEY: <API_KEY>"
```

Note the `http://`, not `https://` - matching what the Arduino actually
sends. See `SIM808_DEBUGGING.md` for the story behind that constraint and
the 404 we hit while confirming the backend accepted it.
