#!/bin/sh
# Keep the known-good public resolver ahead of DHCP-provided router DNS.

case "${1:-}" in
    bound|renew|apply)
        resolv_conf=/etc/resolv.conf
        tmp_file=$(mktemp) || exit 1
        {
            echo "nameserver 223.5.5.5 # meeting-public"
            grep -vE '^[[:space:]]*nameserver[[:space:]]+223\.5\.5\.5([[:space:]]|$)' \
                "$resolv_conf" 2>/dev/null || true
        } >"$tmp_file"
        cat "$tmp_file" >"$resolv_conf"
        rm -f "$tmp_file"
        ;;
esac

