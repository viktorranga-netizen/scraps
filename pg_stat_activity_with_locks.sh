#!/usr/bin/env bash

if [ -s /opt/scripts/cron/cron_vars.sh ]; then
    source /opt/scripts/cron/cron_vars.sh
fi

function check_proxy_for_prod() {
    IS_PROXY_LISTENING=$(lsof -i -P -n | grep LISTEN | grep -c 5441)

    if [[ "$IS_PROXY_LISTENING" -ne 1 ]]; then
        echo "Google SQL proxy is NOT listening, aborting"
        exit 1
    fi
}

# Extract pg_stat_activity from PROD for being able to see if queries linger around for a long while, and pg_locks blocked/blocker pairs so lingering queries can be told apart from queries that are merely stuck waiting on a lock held by something else.

function pg_conn_r() {
    check_proxy_for_prod
    psql -v ON_ERROR_STOP=1 -P format=csv -P csv_fieldsep='`' "host=127.0.0.1 port=5441 dbname=${DB_SCHEMA_PROD} user=${MY_EMAIL} sslmode=disable"
}

# this is INTENTIONALLY using a local db, we do NOT want to pollute the real database with what pg_activity_stat already says!
function pg_conn_w() {
    psql -U postgres -d monitoring
}

function reset_table_prod() {
    # occasionally nuke the table to clear out stale stuff
    echo "TRUNCATE TABLE pg_stat_activity_dump_prod;" | pg_conn_w
    echo "TRUNCATE TABLE pg_lock_waits_dump_prod;" | pg_conn_w
}

function gather_info_prod() {

    #-- pid,extract,query
    #-- 2664688,1791186265,RELEASE SAVEPOINT sa_savepoint_18
    #-- 2665108,1791186275,"SELECT pid, extract(epoch from date_trunc('second', query_start)), query FROM pg_stat_activity WHERE state != 'idle';"

    #CREATE TABLE pg_stat_activity_dump_prod (
    #    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    #    pid INT8 NOT NULL,
    #    created_at INT8 NOT NULL,
    #    query TEXT
    #);

for (( x=0; x<12; x++)); do
        TMP=$(mktemp)

        # some queries might get truncated by removing comments , if it does, it means someone added a comment mid-query
        echo "SELECT pid, extract(epoch from date_trunc('second', query_start)), regexp_replace(regexp_replace(regexp_replace(query, '/\*.*?\*/', '', 'g'), '--[^\r\n]*', '', 'g'), '[\r\n]+', ' ', 'g')  AS query FROM pg_stat_activity WHERE state != 'idle' AND pid != pg_backend_pid();" | pg_conn_r | sed -E 's/\.000000(`)/\1/' | sed -E "s/'/\\\\x27/g" > $TMP

        if [[ "$SEND_EMAIL" -eq 0 ]]; then
            cat $TMP
        fi

        cat $TMP | sed -E 's/\s{2,}/ /g;s/" /"/' | awk -v q="'" 'BEGIN{FS="`"}(NR>1){val=$3; gsub(/^"|"$/,"",val); gsub(/"/,"\x27",val); print "INSERT INTO pg_stat_activity_dump_prod (pid,created_at, query) VALUES (" $1 "," $2 "," q val q ");"}' | pg_conn_w

        rm $TMP

        gather_lock_waits_prod_once

        sleep 5
    done
}

# one snapshot of what query is blocking what, captured alongside the generic query snapshots
function gather_lock_waits_prod_once() {

    #-- blocked_pid,blocked_query,blocking_pid,blocking_query,locktype,blocked_mode,blocking_mode,relation
    #-- 2664688,"UPDATE backlink_domains SET ...",2665108,"BEGIN;",relation,RowExclusiveLock,AccessExclusiveLock,backlink_domains

    #CREATE TABLE pg_lock_waits_dump_prod (
    #    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    #    captured_at BIGINT NOT NULL,
    #    blocked_pid INT8 NOT NULL,
    #    blocked_query TEXT,
    #    blocking_pid INT8 NOT NULL,
    #    blocking_query TEXT,
    #    locktype TEXT,
    #    blocked_mode TEXT,
    #    blocking_mode TEXT,
    #    relation TEXT
    #);

    TMP=$(mktemp)
    CAPTURED_AT=$(date +%s)

    echo "SELECT
    blocked_locks.pid        AS blocked_pid,
    regexp_replace(regexp_replace(regexp_replace(blocked_activity.query, '/\*.*?\*/', '', 'g'), '--[^\r\n]*', '', 'g'), '[\r\n]+', ' ', 'g') AS blocked_query,
    blocking_locks.pid       AS blocking_pid,
    regexp_replace(regexp_replace(regexp_replace(blocking_activity.query, '/\*.*?\*/', '', 'g'), '--[^\r\n]*', '', 'g'), '[\r\n]+', ' ', 'g') AS blocking_query,
    blocked_locks.locktype,
    blocked_locks.mode       AS blocked_mode,
    blocking_locks.mode      AS blocking_mode,
    blocked_locks.relation::regclass AS relation
FROM pg_locks blocked_locks
JOIN pg_stat_activity blocked_activity
    ON blocked_activity.pid = blocked_locks.pid
JOIN pg_locks blocking_locks
    ON blocking_locks.locktype = blocked_locks.locktype
   AND blocking_locks.database   IS NOT DISTINCT FROM blocked_locks.database
   AND blocking_locks.relation   IS NOT DISTINCT FROM blocked_locks.relation
   AND blocking_locks.page       IS NOT DISTINCT FROM blocked_locks.page
   AND blocking_locks.tuple      IS NOT DISTINCT FROM blocked_locks.tuple
   AND blocking_locks.virtualxid IS NOT DISTINCT FROM blocked_locks.virtualxid
   AND blocking_locks.transactionid IS NOT DISTINCT FROM blocked_locks.transactionid
   AND blocking_locks.classid    IS NOT DISTINCT FROM blocked_locks.classid
   AND blocking_locks.objid      IS NOT DISTINCT FROM blocked_locks.objid
   AND blocking_locks.objsubid   IS NOT DISTINCT FROM blocked_locks.objsubid
   AND blocking_locks.pid != blocked_locks.pid
JOIN pg_stat_activity blocking_activity
    ON blocking_activity.pid = blocking_locks.pid
WHERE NOT blocked_locks.granted;" | pg_conn_r | sed -E "s/'/\\\\x27/g" > $TMP

    if [[ "$SEND_EMAIL" -eq 0 ]]; then
        cat $TMP
    fi

    cat $TMP | sed -E 's/\s{2,}/ /g;s/" /"/' | awk -v q="'" -v ts="$CAPTURED_AT" '
BEGIN{FS="`"}
function unwrap(val) {
    gsub(/^"|"$/,"",val)
    gsub(/"/,"\x27",val)
    return val
}
(NR>1){
    blocked_query=unwrap($2)
    blocking_query=unwrap($4)
    locktype=unwrap($5)
    blocked_mode=unwrap($6)
    blocking_mode=unwrap($7)
    relation=unwrap($8)
    print "INSERT INTO pg_lock_waits_dump_prod (captured_at, blocked_pid, blocked_query, blocking_pid, blocking_query, locktype, blocked_mode, blocking_mode, relation) VALUES (" ts "," $1 "," q blocked_query q "," $3 "," q blocking_query q "," q locktype q "," q blocked_mode q "," q blocking_mode q "," q relation q ");"
}' | pg_conn_w

    rm $TMP
}

function analyze_input_from_prod() {

    SUMMARY=$(mktemp)

    echo "SELECT pid,created_at,query FROM pg_stat_activity_dump_prod;" | psql -U postgres -d monitoring --csv | awk '
BEGIN{
    FS=","
}
(NR>1){
    current_time = $2

    if ($1 in last_time) {
        diff = current_time - last_time[$1]

        if (diff > max_diff[$1]) {
            max_diff[$1] = diff
            saved_col[$1] = $3
        }
    }

    last_time[$1] = current_time
}
END {
    print "key,linger,query"
    for (key in max_diff) {
        print key "," max_diff[key] "," saved_col[key]
    }
}' | awk 'BEGIN{FS=","}{if(!($2 ~ /^$/)){print $0}}' > $SUMMARY

    if [[ "$SEND_EMAIL" -eq 1 ]]; then
        echo "Summary of lingering queries" | mutt -s "[[INFO]][pg_stat_activity]" -a $SUMMARY -- ${MY_EMAIL}
    fi

    if [[ "$SEND_EMAIL" -eq 0 ]]; then
        cat $SUMMARY
    fi

    rm $SUMMARY
}

function usage() {
    echo "Usage: $0 [-e] [-r] [-g] [-a] [-h]"
    echo "  -r  reset (truncate) the local dump table before gathering"
    echo "  -g  gather pg_stat_activity info from prod into the local dump table"
    echo "  -a  analyze the gathered info and print lingering queries"
    echo "  -ga to analyze and then gather"
    echo "  -e  to send emails"
    echo "  -h  show this help"
    exit 1
}

SEND_EMAIL=0
DO_RESET=0
DO_GATHER=0
DO_ANALYZE=0
while getopts ":ergah" opt; do
    case "$opt" in
        e) SEND_EMAIL=1 ;;
        r) DO_RESET=1 ;;
        g) DO_GATHER=1 ;;
        a) DO_ANALYZE=1 ;;
        h) usage ;;
        \?) echo "Unknown option: -$OPTARG" >&2; usage ;;
    esac
done

if [[ $OPTIND -eq 1 ]]; then
    usage
fi

[[ "$DO_RESET" -eq 1 ]] && reset_table_prod
[[ "$DO_GATHER" -eq 1 ]] && gather_info_prod
[[ "$DO_ANALYZE" -eq 1 ]] && analyze_input_from_prod
