import {
  ArtifactComment, ArtifactSpec, ArtifactStatus, artifacts, configuration, event, workflow,
} from "runnerless/v1";

const gpu = new ArtifactSpec("public-artifacts",
  new ArtifactStatus("GPU backends", 130678843, "https://pawel.buildbuddy.io"), "gpu-files")
  .file("llm-cc-backend-*-linux-x86_64.bundle", "application/octet-stream", 2147483648)
  .file("llm-cc-backend-*-linux-x86_64.bundle.sha256", "text/plain", 4096)
  .file("llm-cc-backend-*-linux-x86_64.manifest.json", "application/json", 65536);
gpu.comment = ArtifactComment.fileTable("GPU backends");

const comparison = new ArtifactSpec("public-artifacts",
  new ArtifactStatus("Complexity comparison", 130678843, "https://pawel.buildbuddy.io"), "comparison-files")
  .file("comment.md", "text/plain", 24576)
  .file("publication.json", "application/json", 4096)
  .file("report.json", "application/json", 16777216)
  .file("report.md", "text/plain", 16777216)
  .file("baseline.md", "text/plain", 1048576)
  .file("baseline.json", "application/json", 16777216);
comparison.comment = ArtifactComment.generated("comment.md", "publication.json", 24576);

// Each publisher keeps one complete default-branch set and the 14-day fallback.
// The host protects active release inputs and owns storage paths and credentials.
const program = workflow()
  .publishArtifacts("gpu-backends", gpu)
  .publishArtifacts("complexity", comparison);

export function configure(): void {
  program.configure();
  configuration.workflow("artifact-cleanup", ["pull_request"]);
}

export function run(): void {
  program.run();
  if (event.isMerge()) artifacts.cleanup();
}
