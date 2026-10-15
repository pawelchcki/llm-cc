import { badges, BadgeSpec } from "runnerless/v1";
import {
  ArtifactComment, ArtifactSpec, ArtifactStatus, artifacts, workflow,
} from "runnerless/v1";
import {
  capabilities, checks, comments, configuration, event, jobs, packages, repo,
  OperationRef, OperationRequest, PageRequest, RegistryPublishRequest, Value, WriteResult,
} from "runnerless";

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
  .releasePlease()
  .publishArtifacts("gpu-backends", gpu)
  .publishArtifacts("complexity", comparison);

const ratioPrefix = "LM-CC/token data: `";
const publisherActorId: i64 = 317265358;

function commentBody(value: Value): string {
  // The stub runtime retains every allocation. Decode once into UTF-16 units
  // instead of the SDK's repeated string concatenation for long reports.
  const json = value.json;
  if (!json.startsWith('"')) return "";
  const units = new Array<i32>(json.length - 2);
  let length = 0;
  for (let i = 1; i < json.length - 1; i++) {
    let c = json.charCodeAt(i);
    if (c == 92) {
      c = json.charCodeAt(++i);
      if (c == 117) {
        c = I32.parseInt(json.substring(i + 1, i + 5), 16);
        i += 4;
      } else if (c == 110) c = 10;
      else if (c == 114) c = 13;
      else if (c == 116) c = 9;
      else if (c == 98) c = 8;
      else if (c == 102) c = 12;
    }
    units[length++] = c;
  }
  units.length = length;
  return String.fromCharCodes(units);
}

// The escaped LM-CC/token data in a publisher complexity report, given the report
// body as compact JSON text starting at its opening quote: "" when that report has
// no data, or null for any other text.
function ratioData(body: string): string | null {
  // Publication attribution can appear between these two host markers.
  if (!body.startsWith('"<!-- codex-review-artifact-publisher -->')
    || body.indexOf("<!-- ci-toolkit:complexity -->") < 0) return null;
  const start = body.indexOf(ratioPrefix);
  if (start < 0) return "";
  const first = start + ratioPrefix.length;
  const end = body.indexOf("`", first);
  return end < 0 ? "" : body.substring(first, end);
}

// The data in the publisher's complexity report, of which each PR has one; null
// when there is no such report yet.
function publishedData(): string | null {
  let nextPage: i32 = 1;
  while (nextPage > 0) {
    // A page of full artifact reports can exhaust the guest's fuel budget.
    const page = comments.list(new PageRequest(nextPage, 1));
    for (let i = 0; i < page.items.length; i++) {
      const comment = page.items[i];
      // The host serializes compact JSON. Filter its escaped body before scanning
      // publisher fields; unrelated reports also consume the guest's fuel.
      const at = comment.json.indexOf('"body":"<!-- codex-review-artifact-publisher -->');
      if (at < 0) continue;
      const data = ratioData(comment.json.substring(at + 7));
      if (data === null || comment.get("actorId").integer() != publisherActorId) continue;
      return data;
    }
    nextPage = page.nextPage;
  }
  return null;
}

function reportRatio(): void {
  const details = event.details();
  if (details.get("pullNumber").isNull()) return;
  // Another author's comment cannot change the publisher's report. Skip it before
  // any GitHub read: every read counts against the program's deadline. A deletion
  // may have removed the report, so it always rescans the live comments.
  const commented = details.get("event").string() == "issue_comment"
    && details.get("action").string() != "deleted";
  if (commented && details.get("actorId").integer() != publisherActorId) return;
  // The typed PR wrapper also decodes the unused title and description.
  const pr = capabilities.call("pulls.get").require();
  if (pr.get("state").string() != "open") return;
  const prNumber = pr.get("number").integer();
  const prHead = pr.get("head").string();
  const prBase = pr.get("base").string();
  // The host coordinator is checksum-pinned and isolated from PR imports.
  // A PR cannot replace its entrypoint, annotation, or source-selection policy.
  const producers = ["buildbuddy.yaml", "tools/comparison/dogfood.sh",
                     "tools/comparison/ratchet.py", ".llm-cc/rules.json"];
  for (let i = 0; i < producers.length; i++) {
    const path = producers[i];
    if (repo.readText(path, "head") != repo.readText(path, "base")) {
      checks.report("LM-CC/token", false,
        "The comparison producer changed: " + path + "; deploy producer updates through a trusted rollout.");
      return;
    }
  }
  // The publisher's own report arrives with its comment event, unless the event
  // body was cut short; otherwise list comments up to that report.
  let found: string | null = commented ? ratioData(details.get("commentBody").json) : null;
  if (found === null || found == "") found = publishedData();
  const waiting = "Waiting for verified LM-CC/token data for the current PR head and target.";
  if (found === null || found == "") {
    checks.report("LM-CC/token", false, waiting);
    return;
  }
  const data = new Value(commentBody(new Value('"' + found! + '"')));
  const identity = data.get("identity");
  if (data.get("schema_version").integer() != 1
    || identity.get("repository").string() != details.get("fullName").string()
    || identity.get("pr_number").integer() != prNumber
    || identity.get("head_sha").string() != prHead
    || identity.get("target_sha").string() != prBase) {
    checks.report("LM-CC/token", false, waiting);
    return;
  }
  const base = F64.parseFloat(data.get("base_ratio").json);
  const head = F64.parseFloat(data.get("head_ratio").json);
  if (data.get("status").string() != "complete" || !isFinite(base) || !isFinite(head) || base < 0 || head < 0) {
    checks.report("LM-CC/token", false,
      "The current comparison has no complete, finite LM-CC/token measurements.");
    return;
  }
  checks.report("LM-CC/token", head <= base,
    "Repository LM-CC/token: " + base.toString() + " -> " + head.toString()
      + (head <= base ? "; maintained or improved." : "; regression."));
}

export function configure(): void {
  badges.define(new BadgeSpec("badge", "medium", "slowest-job"));
  program.configure();
  configuration.workflow("artifact-cleanup", ["pull_request"]);
  configuration.workflow("registry-publish", ["workflow_run", "runnerless_completion"]);
  configuration.workflow("llm-cc-token-ratio", [
    "pull_request", "status", "runnerless_completion", "issue_comment",
  ]);
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
  reportRatio();
  publishRegistries();
  if (event.isMerge()) artifacts.cleanup();
}
