# Codex environment known issues

These are execution-environment failures seen while building and checking this
repository. Confirm the diagnostic text before using a workaround; a source compile
error is not covered by either case.

## Nordic build cannot write the Zephyr capability cache

**Symptom:** `targets/nordic-zephyr/build.sh` reaches CMake and then emits repeated
errors from `zephyr/cmake/modules/extensions.cmake` such as:

```text
file failed to open for writing (No such file or directory):
/home/davelee/ncs/v3.3.1/zephyr/.cache/ToolchainCapabilityDatabase/...
```

The workspace sandbox can write the repository but not the installed NCS tree. The
failure occurs during compiler capability probing, before it can diagnose application
sources.

**Workaround:** rerun the same build command with sandbox escalation so Zephyr can
write its toolchain cache. Keep the command and working directory unchanged; do not
change permissions in the NCS checkout or move generated files into source. For the
complete Nordic target check:

```sh
./targets/nordic-zephyr/build.sh --all
```

With escalation, Zephyr may report its cache under `~/.cache/zephyr`. Review the
complete build summary and do not describe the sandboxed failure as a target failure.

## LeakSanitizer fails under the sandbox tracer

**Symptom:** Clang fuzz smoke tests or the ASan/UBSan suite fail immediately with:

```text
LeakSanitizer has encountered a fatal error.
LeakSanitizer does not work under ptrace (strace, gdb, etc)
```

The fuzz runner may also write an empty-input `crash-da39a3...` artifact. This is a
LeakSanitizer startup failure under the sandbox tracer, not evidence that the empty
input crashes firmware code.

**Workaround:** stop the sandboxed gate before repeating its long fuzz phase, then
rerun the same `./tools/check.sh` invocation with sandbox escalation. Inspect the
unrestricted GCC, Clang, sanitizer and fuzz results independently. Remove a generated
crash artifact only after confirming it came solely from this LeakSanitizer startup
failure and is not tracked or user-owned.
