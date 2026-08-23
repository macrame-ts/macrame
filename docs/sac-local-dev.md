<!-- Machine setup note, not part of the library. Written 2026-08-22 after Smart App Control
     began blocking freshly linked build output on the author's development machine. -->

# Smart App Control and local test runs

## The problem

Smart App Control blocks unsigned executables it has no Intelligent Security Graph reputation
for. Every relink produces a new hash with no reputation, so freshly built test binaries are
refused - from bash and cmd as well as PowerShell - for an unpredictable interval, measured
between a few minutes and over an hour.

**Builds are never affected. Only execution is.** Compile-time verification keeps working:
`[[nodiscard]]` enumeration, `static_assert`s, the private-slot barriers, template diagnostics.

Diagnostics, if it recurs:

```
# is it on?  1 = enforcing, 0 = evaluation, 2 = off
reg query "HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy" /v VerifiedAndReputablePolicyState

# what got blocked (3077 = blocked, 3076 = audit)
powershell -c "Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-CodeIntegrity/Operational'; Id=3077} -MaxEvents 5 | Format-List TimeCreated, Message"
```

Note the CodeIntegrity log is circular and about 1 MB, so a busy session wraps it within hours -
an empty history means the log rotated, not that nothing happened before.

## What does not work

- **Self-signed certificates.** The signing certificate must chain to a CA in the Microsoft
  Trusted Root Program. A certificate in the local Trusted Root store changes nothing.
- **Exclusions.** Smart App Control has no allowlist, no per-folder exemption, and no Dev Drive
  carve-out, by design.
- **Copying or renaming the binary.** The verdict is per file hash, not per path.
- **Waiting it out reliably.** Sometimes minutes, sometimes never within an hour.
- **Evaluation mode.** It checks without consulting the Intelligent Security Graph, so only
  properly signed apps avoid audit events - and it is unreachable anyway once the setting is on.

## Microsoft's supported options

Exactly three, from the Smart App Control developer documentation:

1. Turn it off while doing local development.
2. Develop on a device or VM where it is not enforcing.
3. Sign every part of the app - exe, dll, scripts, installers - with a certificate from a CA in
   the Trusted Root Program.

Option 3 is poor value here: macrame ships as a source library, so nobody downloads a binary and
the certificate would buy local convenience only.

## Turning it off

**One way.** Smart App Control cannot be turned back on without resetting or reinstalling
Windows. Decide once.

1. Start > **Windows Security**
2. **App & browser control**
3. Under **Smart App Control**, **Smart App Control settings**
4. Set to **Off**, confirm the prompt

No reboot is required; already-running processes are unaffected. Verify:

```
reg query "HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy" /v VerifiedAndReputablePolicyState
# 2 = off
```

Then rerun the suite - the previously blocked binary runs without a rebuild:

```
build/windows-msvc/Release/macrame_playground.exe --tests
```

## If it stays on

Verification still has a path: CI runs the full suite across seven configurations on machines
without Smart App Control, including both Release toolchains, Debug, Shipping, the consumer
package, and ThreadSanitizer. The cost is loop latency, not coverage - which matters most when
chasing an intermittent fault, where local repetition is the tool that works.
