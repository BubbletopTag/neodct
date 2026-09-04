"""The crash guard: what happens after nd-core dies.

This is the code that decides whether a phone comes back up. It lives in
neodct/overlay/bin/nd-crashguard.sh as sourceable functions precisely so it
can be driven here with a stub core and a stub nd-panic, rather than only ever
being tested by making a real phone segfault three times.

The interesting property is the one that is hard to see by reading: crash,
restart, crash, restart forever is worse than the frozen screen this replaces,
so the loop has to stop -- and it has to stop counting when the phone was
actually working in between. Both directions are pinned below.

Time is faked. `guard_uptime` reads NDGUARD_UPTIME_FILE, so the stub core
writes that file to say how long it "ran", which is what lets a two-minute
session take no time at all here.
"""

import os
import signal
import subprocess

import pytest

GUARD_SH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "overlay", "bin", "nd-crashguard.sh",
)

RUN_NEODCT_SH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "overlay", "bin", "run_neodct.sh",
)


def write_exec(path, body):
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(0o755)
    return path


@pytest.fixture
def env(tmp_path):
    """A staged guard: a fake clock, a log, a tty and a recording nd-panic."""
    uptime = tmp_path / "uptime"
    uptime.write_text("100.00 100.00\n")

    calls = tmp_path / "panic.calls"
    panic = write_exec(
        tmp_path / "nd-panic",
        'echo "$*" >> "%s"\nexit 0\n' % calls,
    )

    return {
        "tmp": tmp_path,
        "uptime": uptime,
        "panic": panic,
        "calls": calls,
        "log": tmp_path / "core.log",
        "tty": tmp_path / "tty0",
        "counter": tmp_path / "runs",
    }


def settings(env, **overrides):
    values = {
        "NDGUARD_UPTIME_FILE": env["uptime"],
        "NDGUARD_PANIC": env["panic"],
        "NDGUARD_LOG": env["log"],
        "NDGUARD_TTY": env["tty"],
        "NDGUARD_COUNTDOWN": 0,      # the tests do not need to watch a clock
    }
    values.update(overrides)
    return "".join('%s="%s"\n' % (k, v) for k, v in values.items())


def run(env, commands, timeout=30, **overrides):
    script = settings(env, **overrides) + '. "%s"\n%s\n' % (GUARD_SH, commands)
    return subprocess.run(
        ["sh", "-c", script], capture_output=True, text=True, timeout=timeout
    )


def stub_core(env, durations, status=139):
    """A core that dies `status` after advancing the fake clock.

    `durations` is one number of seconds per invocation; the last is reused
    once the list runs out, so "always crashes instantly" is [0].
    """
    (env["tmp"] / "durations").write_text("\n".join(str(d) for d in durations) + "\n")
    env["counter"].write_text("0\n")
    return write_exec(
        env["tmp"] / "core",
        'n=$(cat "{c}")\n'
        'n=$((n + 1))\n'
        'echo "$n" > "{c}"\n'
        'd=$(sed -n "${{n}}p" "{d}")\n'
        '[ -n "$d" ] || d=$(tail -n 1 "{d}")\n'
        'up=$(cut -d. -f1 "{u}")\n'
        'echo "$((up + d)).00 0.00" > "{u}"\n'
        'exit {s}\n'.format(c=env["counter"], d=env["tmp"] / "durations",
                            u=env["uptime"], s=status),
    )


def runs(env):
    return int(env["counter"].read_text().strip())


def panic_calls(env):
    if not env["calls"].exists():
        return []
    return [line for line in env["calls"].read_text().splitlines() if line]


# --- reading the clock ----------------------------------------------------


def test_uptime_is_whole_seconds_from_proc(env):
    env["uptime"].write_text("3241.87 12963.55\n")
    assert run(env, "guard_uptime").stdout.strip() == "3241"


def test_uptime_of_a_phone_with_no_proc_is_zero(env):
    result = run(env, "guard_uptime", NDGUARD_UPTIME_FILE=env["tmp"] / "nope")
    assert result.stdout.strip() == "0"


def test_uptime_refuses_to_believe_nonsense(env):
    """A garbled /proc/uptime must not make every crash look like a long run."""
    env["uptime"].write_text("not-a-number\n")
    assert run(env, "guard_uptime").stdout.strip() == "0"


# --- counting -------------------------------------------------------------


def test_a_fast_death_extends_the_streak(env):
    assert run(env, "guard_next_count 0 4").stdout.strip() == "1"
    assert run(env, "guard_next_count 1 4").stdout.strip() == "2"
    assert run(env, "guard_next_count 2 0").stdout.strip() == "3"


def test_a_long_run_starts_the_count_again(env):
    """It is still a crash -- it is just not one of a series, so it is one."""
    assert run(env, "guard_next_count 2 120").stdout.strip() == "1"
    assert run(env, "guard_next_count 9 999").stdout.strip() == "1"


def test_the_healthy_threshold_is_where_it_says_it_is(env):
    assert run(env, "guard_next_count 1 119").stdout.strip() == "2"
    assert run(env, "guard_next_count 1 120").stdout.strip() == "1"


def test_halting_is_the_third_consecutive_crash(env):
    assert run(env, "guard_should_halt 2 && echo halt || echo go").stdout.strip() == "go"
    assert run(env, "guard_should_halt 3 && echo halt || echo go").stdout.strip() == "halt"


# --- the loop -------------------------------------------------------------


def test_a_core_that_always_crashes_is_given_three_tries_and_no_more(env):
    core = stub_core(env, [0])
    result = run(env, "guard_supervise; echo rc=$?", NDGUARD_CORE=core)

    assert "rc=1" in result.stdout          # gave up
    assert runs(env) == 3
    calls = panic_calls(env)
    assert len(calls) == 3
    assert calls[0].startswith("--seconds")
    assert calls[1].startswith("--seconds")
    assert calls[2].startswith("--halt")    # the last screen stays up


def test_the_screen_is_told_which_try_this_was(env):
    core = stub_core(env, [0])
    run(env, "guard_supervise", NDGUARD_CORE=core)

    calls = panic_calls(env)
    assert "--crash 1 --limit 3" in calls[0]
    assert "--crash 2 --limit 3" in calls[1]
    assert "--crash 3 --limit 3" in calls[2]
    assert "--status 139" in calls[0]


def test_a_working_phone_in_between_starts_the_count_again(env):
    """Two fast crashes, a two-minute session, then fast crashes again.

    Without the reset the third invocation -- the long, healthy one -- would
    be crash three and halt the phone, which is precisely the wrong answer:
    the phone had just worked for two minutes. With it, that death is crash
    one of a new streak, so two more invocations follow before it gives up.
    """
    core = stub_core(env, [0, 0, 200, 0, 0, 0])
    result = run(env, "guard_supervise; echo rc=$?", NDGUARD_CORE=core)

    assert "rc=1" in result.stdout
    assert runs(env) == 5
    assert panic_calls(env)[-1].startswith("--halt")
    # The long run is the one that reset it: crash 1 is reached twice.
    assert env["log"].read_text().count("crash 1 of 3") == 2


def test_a_clean_exit_gets_no_crash_screen_but_is_still_counted(env):
    """nd-core exiting 0 in a tight loop spins as hard as one that faults."""
    core = stub_core(env, [0], status=0)
    result = run(env, "guard_supervise; echo rc=$?", NDGUARD_CORE=core)

    assert "rc=1" in result.stdout
    assert runs(env) == 3
    calls = panic_calls(env)
    assert len(calls) == 1                  # only the final halt screen
    assert calls[0].startswith("--halt")
    assert "--status 0" in calls[0]


def test_a_signal_stops_the_loop_instead_of_restarting(env):
    """A poweroff must not be answered by booting one more time."""
    core = write_exec(
        env["tmp"] / "core",
        'n=$(cat "{c}"); n=$((n + 1)); echo "$n" > "{c}"\n'
        '[ "$n" -lt 2 ] || kill -TERM "$PPID"\n'
        'exit 0\n'.format(c=env["counter"]),
    )
    env["counter"].write_text("0\n")

    result = run(env, "guard_supervise; echo rc=$?", NDGUARD_CORE=core)

    assert "rc=0" in result.stdout          # asked to stop, not given up
    assert runs(env) == 2
    assert panic_calls(env) == []
    assert "not restarting" in env["log"].read_text()


def test_every_attempt_is_written_down(env):
    """The screen can only show the crash happening now; the log has the rest."""
    core = stub_core(env, [0])
    run(env, "guard_supervise", NDGUARD_CORE=core)

    log = env["log"].read_text()
    assert "crash 1 of 3" in log
    assert "crash 2 of 3" in log
    assert "crash 3 of 3" in log
    assert "exited 139" in log


# --- the fallback ---------------------------------------------------------


def test_a_missing_nd_panic_still_puts_something_on_the_screen(env):
    core = stub_core(env, [0])
    result = run(env, "guard_supervise; echo rc=$?", NDGUARD_CORE=core,
                 NDGUARD_PANIC=env["tmp"] / "not-installed")

    assert "rc=1" in result.stdout
    assert runs(env) == 3
    tty = env["tty"].read_text()
    assert "CORE SYSTEM CRASHED" in tty
    assert "NOT RESTARTING" in tty


def test_an_nd_panic_that_cannot_reach_the_framebuffer_falls_back(env):
    """Exit 1 from nd-panic means nothing was drawn -- see nd_panic.h."""
    broken = write_exec(env["tmp"] / "broken-panic", "exit 1\n")
    core = stub_core(env, [0])
    run(env, "guard_supervise", NDGUARD_CORE=core, NDGUARD_PANIC=broken)

    assert "CORE SYSTEM CRASHED" in env["tty"].read_text()


def test_the_fallback_banner_does_the_waiting_itself(env):
    """nd-panic sleeps through its own countdown; the banner cannot, so it must.

    A restart with no pause at all would flash the message past unread.
    """
    core = stub_core(env, [0])
    result = run(env, 'guard_supervise; echo rc=$?', NDGUARD_CORE=core,
                 NDGUARD_PANIC=env["tmp"] / "not-installed",
                 NDGUARD_COUNTDOWN=1, timeout=30)

    assert "rc=1" in result.stdout
    # Two restarts at one second each; the halt screen does not wait.
    assert "RESTARTING" in env["tty"].read_text()


# --- the boot script agrees with the guard --------------------------------


def test_the_boot_script_sources_the_guard_and_names_the_core(tmp_path):
    body = open(RUN_NEODCT_SH).read()
    assert ". /bin/nd-crashguard.sh" in body
    assert "NDGUARD_CORE=/NeoDCT/System/bin/nd-core" in body
    assert "guard_supervise" in body


def test_the_boot_script_no_longer_truncates_the_app_crash_log(tmp_path):
    """`nd-core 2> crash.log` destroyed every application crash on every boot.

    nd_crash.c appends its reports to that file, and a truncating redirect
    opened for the whole session both wiped the history and then overwrote
    whatever was appended afterwards.
    """
    code = [line for line in open(RUN_NEODCT_SH).read().splitlines()
            if not line.lstrip().startswith("#")]
    assert not any("crash.log" in line for line in code), \
        "nd-core's stderr must not share a file with nd_crash.c's reports"
    assert any("CORE_LOG=/NeoDCT/User/logs/core.log" in line for line in code)
    assert '2>> "$NDGUARD_LOG"' in open(GUARD_SH).read()


def test_both_scripts_parse_as_posix_shell():
    for path in (GUARD_SH, RUN_NEODCT_SH):
        assert subprocess.run(["sh", "-n", path]).returncode == 0, path


def test_the_guard_does_nothing_merely_by_being_sourced(tmp_path):
    """ndsys-apply.sh's rule, for the same reason: a sourced file that acts is
    a file the tests cannot look at one function at a time."""
    marker = tmp_path / "ran"
    result = subprocess.run(
        ["sh", "-c", 'NDGUARD_TTY="%s"\n. "%s"\n' % (marker, GUARD_SH)],
        capture_output=True, text=True,
    )
    assert result.returncode == 0
    assert result.stdout == ""
    assert not marker.exists()


def test_signal_names_the_guard_traps_are_the_ones_init_sends():
    body = open(GUARD_SH).read()
    assert "trap 'NDGUARD_STOPPING=1' TERM INT HUP" in body
    assert signal.SIGTERM.name == "SIGTERM"
