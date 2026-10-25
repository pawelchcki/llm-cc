import { badges, BadgeSpec, scans, ScanSpec } from "runnerless/v1";
import {
  ArtifactComment, ArtifactSpec, ArtifactStatus, artifacts, workflow,
} from "runnerless/v1";
import {
  checks, configuration, event, jobs, packages, repo,
  OperationRef, OperationRequest, RegistryPublishRequest, WriteResult,
} from "runnerless";

const gpu = new ArtifactSpec("public-artifacts",
  new ArtifactStatus("GPU backends", 130678843, "https://pawel.buildbuddy.io"), "gpu-files")
  .file("llm-cc-backend-*-linux-x86_64.bundle", "application/octet-stream", 2147483648)
  .file("llm-cc-backend-*-linux-x86_64.bundle.sha256", "text/plain", 4096)
  .file("llm-cc-backend-*-linux-x86_64.manifest.json", "application/json", 65536);
gpu.comment = ArtifactComment.fileTable("GPU backends");

// Public CI publishes build artifacts. LM-CC complexity is scored by the organization's
// installed llm-cc scanners (Runnerless content scans): each blob is scored once, a pull
// request scores only the files it changes, and the ratchet fails a rise in LM-CC/token.
const complexity = new ScanSpec("complexity", "llm-cc", true);
const program = workflow()
  .releasePlease()
  .publishArtifacts("gpu-backends", gpu);

export function configure(): void {
  configuration.titlePolicy();
  badges.define(new BadgeSpec("badge", "medium", "slowest-job"));
  scans.define(complexity);
  program.configure();
  configuration.workflow("artifact-cleanup", ["pull_request"]);
  configuration.workflow("registry-publish", ["workflow_run", "runnerless_completion"]);
}

function reportPublishing(key: string, name: string): void {
  const result = jobs.lookup<WriteResult>(new OperationRef<WriteResult>("op:" + key));
  if (!result.ok) return;
  const outcome = result.value;
  const value = outcome.get("value");
  let summary = value.get("error").string();
  if (summary == "") summary = value.get("state").string();
  if (summary == "") summary = outcome.get("status").string();
  checks.report(name, outcome.get("status").string() == "succeeded" &&
    !value.get("failed").boolean(), summary);
}

function publishRegistries(): void {
  if (event.get("event") == "runnerless_completion") {
    reportPublishing("npm-publish", "npm publish");
    reportPublishing("pypi-publish", "PyPI publish");
    return;
  }
  const details = event.details();
  if (event.get("event") != "workflow_run" || details.get("action").string() != "completed" ||
      details.get("workflowPath").string() != ".github/workflows/release.yml" ||
      details.get("conclusion").string() != "success" || !details.get("pullNumber").isNull()) return;
  const version = repo.readText("version.txt", "head").trim();
  const tag = "v" + version;
  // Dry runs have no published release. The host binds the public release/tag
  // to this workflow run's immutable head before it reads any package assets.
  if (!packages.releasePublished(tag, version)) return;
  const npm = new RegistryPublishRequest(tag, version);
  const targets = ["linux-x64", "linux-arm64", "darwin-arm64", "darwin-x64", "win32-x64"];
  for (let i = 0; i < targets.length; i++) {
    npm.add("@pawelchcki/llm-cc-" + targets[i], "pawelchcki-llm-cc-" + targets[i] + "-" + version + ".tgz");
  }
  npm.add("llm-cc", "llm-cc-" + version + ".tgz");
  packages.npm(new OperationRequest("npm-publish"), npm);
  const pypi = new RegistryPublishRequest(tag, version);
  const platforms = ["manylinux_2_28_x86_64", "manylinux_2_35_aarch64", "macosx_14_0_arm64",
                     "macosx_14_0_x86_64", "win_amd64"];
  for (let i = 0; i < platforms.length; i++) {
    pypi.add("llm-cc", "llm_cc-" + version + "-py3-none-" + platforms[i] + ".whl");
  }
  packages.pypi(new OperationRequest("pypi-publish"), pypi);
}

export function run(): void {
  program.run();
  scans.publish(complexity, "LM-CC/token");
  publishRegistries();
  if (event.isMerge()) artifacts.cleanup();
}
