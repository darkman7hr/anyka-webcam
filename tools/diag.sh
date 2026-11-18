#!/bin/bash
# Live checks on the running UI: headers, request log, preset round trip.
cd "$(dirname "$0")" || exit 1
T=$(cat .token)
# Which port ours answers on moves when the UI is promoted onto 8080, so read
# it from the file deploy.py maintains instead of hard-coding it.
P=$(cat .port 2>/dev/null || echo 8080)
B="http://192.168.10.46:${P}/cgi-bin/api"

echo '=== response headers a browser receives ==='
curl -s -i -m 10 "$B?act=status&token=$T" | head -8

echo
echo '=== does a mutation get logged? ==='
curl -s -m 10 "$B?act=goto&pan=250&tilt=110&token=$T" > /dev/null
curl -s -m 10 "$B?act=preset_save&slot=9&name=diag%20probe&token=$T" > /dev/null
curl -s -m 10 "$B?act=preset_del&slot=9&token=$T" > /dev/null
curl -s -m 10 "$B?act=log&token=$T"

echo
echo '=== preset list (must NOT be cached) ==='
curl -s -m 10 "$B?act=presets&token=$T"

echo
echo '=== static file headers ==='
curl -s -I -m 10 "http://192.168.10.46:${P}/app.js?v=6" | head -6
