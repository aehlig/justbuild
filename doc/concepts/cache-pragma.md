Action caching pragma
=====================

Introduction: exit code, build failures, and caching
----------------------------------------------------

The exit code of a process is used to signal success or failure of that
process. By convention, 0 indicates success and any other value
indicates some form of failure.

Our tool expects all build actions to follow this convention. A non-zero
exit code of a regular build action has two consequences.

 - As the action failed, the whole build is aborted and considered
   failed.
 - As such a failed action can never be part of a successful build, it
   is (effectively) not cached.

This non-caching is achieved by rerequesting an action without cache
look up, if a failed action from cache is reported.

In particular, for building, we have the property that everything that
does not lead to aborting the build can (and will) be cached. This
property is justified as we expect build actions to behave in a
functional way.

Test and run actions
--------------------

Tests have a lot of similarity to regular build actions: a process is
run with given inputs, and the results are processed further (e.g., to
create reports on test suites). However, they break the above described
connection between caching and continuation of the build: we expect that
some tests might be flaky (even though they shouldn't be, of course)
and hence only want to cache successful tests. Nevertheless, we do want
to continue testing after the first test failure.

Another breakage of the functionality assumption of actions are "run"
actions, i.e., local actions that are executed either because of their
side effect on the host system, or because of their non-deterministic
results (e.g., monitoring some resource). Those actions should never be
cached, but if they fail, the build should be aborted.

Tainting
--------

Targets that, directly or indirectly, depend on non-functional actions
are not regular targets. They are test targets, run targets, benchmark
results, etc; in any case, they are tainted in some way. When adding
high-level caching of targets, we will only support caching for
untainted targets.

To make everybody aware of their special nature, they are clearly marked
as such: tainted targets not generated by a tainted rule (e.g., a test
rule) have to explicitly state their taintedness in their attributes.
This declaration also gives a natural way to mark targets that are
technically pure, but still should be used only in test, e.g., a mock
version of a larger library.

Besides being for tests only, there might be other reasons why a target
might not be fit for general use, e.g., configuration files with
accounts for developer access, or files under restrictive licences. To
avoid having to extend the framework for each new use case, we allow
arbitrary strings as markers for the kind of taintedness of a target. Of
course, a target can be tainted in more than one way.

More precisely, rules can have `"tainted"` as an additional property.
Moreover `"tainted"` is another reserved keyword for target arguments
(like `"type"` and `"arguments_config"`). In both cases, the value has
to be a list of strings, and the empty list is assumed, if not
specified.

A rule is tainted with the set of strings in its `"tainted"` property. A
target is tainted with the union of the set of strings of its
`"tainted"` argument and the set of strings its generating rule is
tainted with.

Every target has to be tainted with (at least) the union of what its
dependencies are tainted with.

For tainted targets, the `analyse`, `build`, and `install` commands
report the set of strings the target is tainted with.

### `"may_fail"` and `"no_cache"` properties of `"ACTION"`

The `"ACTION"` function in the defining expression of a rule have two
additional (besides inputs, etc) parameters `"may_fail"` and
`"no_cache"`. Those are not evaluated and have to be lists of strings
(with empty assumed if the respective parameter is not present). Only
strings the defining rule is tainted with may occur in that list. If the
list is not empty, the corresponding may-fail or no-cache bit of the
action is set.

For actions with the `"may_fail"` bit set, the optional parameter
`"fail_message"` with default value `"action failed"` is evaluated. That
message will be reported if the action returns a non-zero exit value.

Actions with the no-cache bit set are never cached. If an action with
the may-fail bit set exits with non-zero exit value, the build is
continued if the action nevertheless managed to produce all expected
outputs. We continue to ignore actions with non-zero exit status from
cache.

### Marking of failed artifacts

To simplify finding failures in accumulated reports, our tool keeps
track of artifacts generated by failed actions. More precisely,
artifacts are considered failed if one of the following conditions
applies.

 - Artifacts generated by failed actions are failed.
 - Tree artifacts containing a failed artifact are failed.
 - Artifacts generated by an action taking a failed artifact as input
   are failed.

The identifiers used for built artifacts (including trees) remain
unchanged; in particular, they will only describe the contents and not
if they were obtained in a failed way.

When reporting artifacts, e.g., in the log file, an additional marker is
added to indicate that the artifact is a failed one. After every `build`
or `install` command, if the requested artifacts contain failed one, a
different exit code is returned.

### The `install-cas` subcommand

A typical workflow for testing is to first run the full test suite and
then only look at the failed tests in more details. As we don't take
failed actions from cache, installing the output can't be done by
rerunning the same target as `install` instead of `build`. Instead, the
output has to be taken from CAS using the identifier shown in the build
log. To simplify this workflow, there is the `install-cas` subcommand
that installs a CAS entry, identified by the identifier as shown in the
log to a given location or (if no location is specified) to `stdout`.