#!/usr/bin/env bash
# Claude Code statusLine hook — reads quota JSON from stdin, writes /tmp/claude-quota.json

INPUT=$(cat)

FIVE_PCT=$(printf '%s' "$INPUT"    | jq -r '.rate_limits.five_hour.used_percentage // 0')
FIVE_RESET=$(printf '%s' "$INPUT"  | jq -r '.rate_limits.five_hour.resets_at // 0')
WEEKLY_PCT=$(printf '%s' "$INPUT"  | jq -r '.rate_limits.seven_day.used_percentage // 0')
WEEKLY_RESET=$(printf '%s' "$INPUT"| jq -r '.rate_limits.seven_day.resets_at // 0')

printf '{"five_hour":{"used_percentage":%s,"resets_at":%s},"seven_day":{"used_percentage":%s,"resets_at":%s}}\n' \
    "$FIVE_PCT" "$FIVE_RESET" "$WEEKLY_PCT" "$WEEKLY_RESET" > /tmp/claude-quota.json

printf '5h:%.0f%% 7d:%.0f%%' "$FIVE_PCT" "$WEEKLY_PCT"
