# TODO

## Score related files as one context

The project score currently adds independently scored files. Add a separate
context-group score that evaluates related files in one model context, while
retaining per-file scores for comparisons and hotspot reporting.

Joint scoring should differ from adding file scores. Definitions, imports,
interfaces, tests, and repeated naming patterns in earlier files can change the
entropy of later files. Cross-file indirection may also create complexity that
isolated analysis misses. The new score should approximate the working set an
agent needs to understand or modify code.

Choosing useful groups is a central part of the feature and may be best handled
by the project's build system. Bazel targets already identify meaningful source
collections and their dependencies. Explore using direct target sources by
default, with clear policies for dependencies, tests, and generated sources.
Support explicit user-defined groups for other build systems or repositories
without one.

Record the grouping source, use deterministic file ordering and explicit file
boundaries, define stable behavior for groups larger than the context window,
and validate whether the metric predicts agent effort better than additive file
scores.
