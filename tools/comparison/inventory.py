import fnmatch
import hashlib
import subprocess

from .common import digest

LANGUAGES = {
    ".c": "c",
    ".h": "c",
    ".cc": "cpp",
    ".cpp": "cpp",
    ".cxx": "cpp",
    ".c++": "cpp",
    ".cu": "cpp",
    ".hh": "cpp",
    ".hpp": "cpp",
    ".hxx": "cpp",
    ".h++": "cpp",
    ".cuh": "cpp",
    ".py": "python",
    ".pyw": "python",
    ".pyi": "python",
    ".rs": "rust",
    ".go": "go",
    ".java": "java",
    ".js": "javascript",
    ".mjs": "javascript",
    ".cjs": "javascript",
    ".cs": "csharp",
    ".csx": "csharp",
}


class GitError(RuntimeError):
    pass


def _git(repo, args, data=None):
    completed = subprocess.run(
        ["git", "-C", str(repo), *args],
        input=data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode:
        raise GitError(
            "git %s failed: %s"
            % (" ".join(args), completed.stderr.decode("utf-8", "replace").strip())
        )
    return completed.stdout


def merge_base(repo, target, head):
    try:
        value = _git(repo, ["merge-base", target, head]).strip().decode("ascii")
    except GitError as exc:
        raise GitError(
            "cannot find merge base for target %s and head %s; fetch the target branch and deepen the checkout: %s"
            % (target, head, exc)
        ) from exc
    if not value:
        raise GitError(
            "git returned an empty merge base; fetch complete target and head history"
        )
    return value


def resolve_commit(repo, revision):
    try:
        value = (
            _git(repo, ["rev-parse", "--verify", "%s^{commit}" % revision])
            .strip()
            .decode("ascii")
        )
    except GitError as exc:
        raise GitError(
            "cannot resolve revision %s to a commit; fetch the required history: %s"
            % (revision, exc)
        ) from exc
    if len(value) != 40:
        raise GitError("git returned an invalid commit id for %s" % revision)
    return value


def _extension(path):
    name = path.rsplit("/", 1)[-1]
    dot = name.rfind(".")
    return name[dot:].lower() if dot >= 0 else ""


def _classify(path, rules):
    test_patterns = rules.get(
        "tests",
        [
            "test/**",
            "tests/**",
            "testdata/**",
            "fixtures/**",
            "fuzz/**",
            "fuzzers/**",
            "**/test/**",
            "**/tests/**",
            "**/testdata/**",
            "**/fixtures/**",
            "**/fuzz/**",
            "**/fuzzers/**",
            "*_test.*",
            "test_*.*",
            "**/*_test.*",
            "**/test_*.*",
        ],
    )
    tooling = rules.get(
        "tooling",
        ["tools/**", "examples/**", "example/**", "scripts/**", "benchmarks/**"],
    )
    if any(fnmatch.fnmatchcase(path, p) for p in test_patterns):
        return "tests"
    if any(fnmatch.fnmatchcase(path, p) for p in tooling):
        return "tooling"
    return "runtime"


def inventory(repo, revision, fingerprint, rules=None, max_file_bytes=65536):
    rules = rules or {}
    raw = _git(repo, ["ls-tree", "-rlz", "--full-tree", revision])
    records = []
    blob_ids = []
    for entry in raw.split(b"\0"):
        if not entry:
            continue
        metadata, path_bytes = entry.split(b"\t", 1)
        mode, kind, object_id, size_text = metadata.split()
        path = path_bytes.decode("utf-8", "surrogateescape")
        size = None if size_text == b"-" else int(size_text)
        record = {
            "path": path,
            "language": None,
            "category": _classify(path, rules),
            "size": size,
            "content_sha256": None,
            "key": None,
            "scorable": False,
            "reason": None,
            "_object": object_id.decode("ascii"),
        }
        if kind == b"blob":
            record["language"] = (LANGUAGES | rules.get("extensions", {})).get(
                _extension(path)
            )
        if mode == b"160000" or kind == b"commit":
            record["reason"] = "submodule"
        elif mode == b"120000":
            record["reason"] = "symlink"
        elif kind != b"blob":
            record["reason"] = "unsupported"
        elif any(fnmatch.fnmatchcase(path, p) for p in rules.get("exclude", [])):
            record["reason"] = "excluded"
        elif record["language"] is None:
            record["reason"] = "unsupported"
        elif size is not None and size > max_file_bytes:
            record["reason"] = "oversized"
        else:
            blob_ids.append(record["_object"])
        records.append(record)
    blobs = read_blobs(repo, blob_ids)
    for record in records:
        if record["reason"] is None and record["_object"] in blobs:
            content = blobs[record["_object"]]
            sha = hashlib.sha256(content).hexdigest()
            record.update(
                content_sha256=sha,
                key=digest([sha, record["language"], fingerprint]),
                scorable=True,
            )
        record.pop("_object")
    return records, {
        hashlib.sha256(value).hexdigest(): value for value in blobs.values()
    }


def read_blobs(repo, object_ids):
    unique = list(dict.fromkeys(object_ids))
    if not unique:
        return {}
    process = subprocess.Popen(
        ["git", "-C", str(repo), "cat-file", "--batch"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    output, error = process.communicate(("\n".join(unique) + "\n").encode("ascii"))
    if process.returncode:
        raise GitError(
            "git cat-file --batch failed: " + error.decode("utf-8", "replace")
        )
    found = {}
    offset = 0
    for expected in unique:
        end = output.index(b"\n", offset)
        header = output[offset:end].split()
        offset = end + 1
        if len(header) != 3 or header[0].decode() != expected or header[1] != b"blob":
            raise GitError("unexpected git cat-file response")
        size = int(header[2])
        found[expected] = output[offset : offset + size]
        offset += size
        if output[offset : offset + 1] != b"\n":
            raise GitError("malformed git cat-file response")
        offset += 1
    return found


def changes(repo, base, head):
    fields = _git(
        repo, ["diff", "--name-status", "-z", "--find-renames", base, head]
    ).split(b"\0")
    result = []
    index = 0
    while index < len(fields) and fields[index]:
        status = fields[index].decode("ascii")
        index += 1
        if status.startswith(("R", "C")):
            old, new = fields[index : index + 2]
            index += 2
        else:
            new = fields[index]
            index += 1
            old = new

        def decode(p):
            return p.decode("utf-8", "surrogateescape")

        result.append(
            {
                "status": status,
                "old_path": None if status.startswith("A") else decode(old),
                "new_path": None if status.startswith("D") else decode(new),
            }
        )
    return result
