# Compile every header under include/photon/ on its own, so a header that is not
# self-contained fails here instead of in whichever .cpp happens to include it
# second.  Up to JOBS compilations run at once, defaulting to the number of
# online CPUs; JOBS=1 gives the old serial behaviour.
#
# Extra arguments are passed through to gcc, which is how you supply a dependency
# this machine keeps somewhere off the default search path:
#   test/test-includes.sh -I/opt/homebrew/include
#
# Exits non-zero if any header failed.
cd `dirname $0`/../include || exit 1

# getconf rather than nproc: nproc is GNU coreutils and macOS does not have it.
# The case arm catches both a machine where getconf is missing and a JOBS that
# was set to something xargs -P would reject.
JOBS=${JOBS:-`getconf _NPROCESSORS_ONLN 2>/dev/null`}
case $JOBS in ('' | *[!0-9]*) JOBS=1 ;; esac

# Force colour ONLY when this script's own stdout is a terminal; into a
# redirected log the escape bytes are just noise, so COLOR stays empty and gcc is
# invoked exactly as before.
#
# Why forcing is needed at all: gcc and clang colour diagnostics only when their
# own output is a TTY, and this script captures them through a pipe so each can
# be attributed to its header -- which silently drops the colour.  Which flag
# forces it back on is not portable: gcc 15 ignores -fcolor-diagnostics and clang
# older than 17 does not know -fdiagnostics-color, so probe for the flag that
# actually puts an escape byte into a pipe rather than guessing from a version
# number.
COLOR=
if [ -t 1 ]; then
    esc=`printf '\033'`
    for f in -fdiagnostics-color=always -fcolor-diagnostics; do
        if printf 'int x = "s";\n' | gcc -x c - -c -o /dev/null "$f" 2>&1 | grep -q "$esc"; then
            COLOR=$f
            break
        fi
    done
fi

# xargs appends its own items after the command, so the gcc flags cannot be
# handed to `sh -c` as leading arguments; they travel in the environment instead.
# The worker expands this unquoted on purpose -- it is a flag list, not one
# argument.  $COLOR goes first so that passing -fdiagnostics-color=never on the
# command line still turns the colour back off.  That spelling, not clang's
# -fno-color-diagnostics: gcc rejects the latter outright with "unrecognized
# command-line option".
PHOTON_HDR_FLAGS="$COLOR $*"
export PHOTON_HDR_FLAGS

result=`mktemp`
trap 'rm -f "$result"' EXIT

# Each header's diagnostics are captured and then printed under its own FAIL
# line.  Letting gcc write straight to the terminal was fine serially, but with
# -P 8 the streams interleave and an error can no longer be attributed to the
# header that produced it.
find photon -name "*.h" -print0 |
    xargs -0 -P "$JOBS" -n1 sh -c '
        h=$1
        if err=`printf "#include <%s>\n" "$h" |
                gcc -x c++ - -std=c++20 -c -I . -o /dev/null $PHOTON_HDR_FLAGS 2>&1`; then
            printf "PASS %s\n" "$h"
        else
            printf "FAIL %s\n" "$h"
            printf "%s\n" "$err" | sed "s/^/      /"
        fi
    ' _ | tee "$result"

pass=`grep -c "^PASS " "$result"`
fail=`grep -c "^FAIL " "$result"`
printf "\n%d headers: %d passed, %d failed (JOBS=%d)\n" \
    "$((pass + fail))" "$pass" "$fail" "$JOBS"
[ "$fail" -eq 0 ]
