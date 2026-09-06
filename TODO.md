# TODO

## Keep a model loaded across analysis requests

Repeated CLI invocations currently reload model weights and initialize the
backend whenever inference is needed. Explore an opt-in local worker that
keeps a model loaded and accepts analysis or scoring requests from the CLI,
amortizing startup cost across short runs and incremental edits.

Reuse a worker only when the model content (including every GGUF shard),
inference ABI, backend, and GPU offload settings match. Reuse or resize inference
contexts as needed, while clearing token and KV state between independent
requests. Preserve entropy-cache lookups so warm cache hits can complete
without starting a worker or loading a model.

Define request queuing and cancellation, coordinate GPU ownership with existing
standalone processes, and bound resident memory with an idle timeout and an
explicit shutdown command. Report when a request is queued, loading a model,
or reusing one, and handle worker crashes and stale endpoints cleanly.

Benchmark cold startup, repeated requests, and incremental project analysis
against the current CLI. Verify identical scoring and cache behavior, and
measure the latency benefit alongside idle RAM/VRAM use before choosing
defaults.

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
