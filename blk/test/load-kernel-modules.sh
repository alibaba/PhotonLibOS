#!/bin/sh
# Load the kernel modules the blk transports need and print what the test binaries
# will actually see. Called from ci.linux.x86_64.yml before the build.
#
# tcmu, ublk and vduse gate every case on a kernel-side prerequisite and skip when it
# is absent -- but the skip is a LOG_INFO line, because report_skip() never tells gtest
# -- so ctest counts those cases as passed while they did nothing. Loading the modules
# here makes the run's own log say which of them came up, and warns when none did.
#
# Every action prints what it ran and what came back, including modprobe's own words on
# failure: "absent from /lib/modules" and "present but refused" need different fixes,
# and a silent `2>/dev/null` cannot tell them apart.
#
# Never fatal, and never installs anything: an image without these modules still runs
# the other ~230 cases, and a test step is the wrong place to change the machine.
[ "$(uname -s)" = Linux ] || { echo "not Linux, nothing to load"; exit 0; }

for m in ublk_drv vduse virtio_vdpa target_core_mod target_core_user tcm_loop; do
    # Ask first whether the driver is already there, and only load what is missing.
    # /sys/module/<name> exists for built-in code as well as for loaded modules; the
    # refcnt file exists only for the latter, so the two are distinguishable without
    # running modprobe at all.
    if [ -e "/sys/module/$m/refcnt" ]; then
        printf '%-16s already loaded\n' "$m"
    elif [ -d "/sys/module/$m" ]; then
        printf '%-16s builtin\n' "$m"
    else
        printf 'modprobe %-16s ' "$m"
        if err=$(sudo modprobe "$m" 2>&1); then
            echo "ok"
        else
            echo "FAILED: $(printf '%s' "$err" | tr '\n' ' ')"
        fi
    fi
done

if grep -q ' /sys/kernel/config ' /proc/mounts; then
    echo "configfs /sys/kernel/config   already mounted"
else
    printf 'mount -t configfs /sys/kernel/config   '
    if err=$(sudo mount -t configfs none /sys/kernel/config 2>&1); then
        echo "ok"
    else
        echo "FAILED: $(printf '%s' "$err" | tr '\n' ' ')"
    fi
fi

echo "--- prerequisites the blk suites check ---"
ls -l /dev/ublk-control /dev/vduse/control 2>&1 || true
ls -d /sys/kernel/config/target/core 2>&1 || true
if command -v vdpa >/dev/null; then echo "vdpa tool                     present"; else echo "vdpa tool                     MISSING"; fi

n=0
[ -e /dev/ublk-control ] && n=$((n + 1)) || true
[ -e /dev/vduse/control ] && n=$((n + 1)) || true
[ -d /sys/kernel/config/target/core ] && n=$((n + 1)) || true
echo "prerequisites present: $n/3"

if [ "$n" -eq 0 ]; then
    echo "::warning::no privileged blk transport came up; tcmu, ublk and vduse cases will be counted as passed without running"
fi
exit 0
