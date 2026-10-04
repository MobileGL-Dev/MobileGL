# pbkill.sh - stop every benchmark helper and glmark2 (phone, root)
for p in $(ps -A -o pid,args | grep -E "pbbatch|pbcool|pbsamp|stfix|profrun|pbprof" | grep -v grep | grep -v pbkill | awk '{print $1}'); do kill $p 2>/dev/null; done
pkill -x glmark2-es2-wayland; pkill -x glmark2-wayland; pkill -x glmark2-es2-way; pkill -x glmark2-wayland
sleep 1; ps -A -o pid,args | grep -E "pbbatch|glmark2" | grep -v grep
