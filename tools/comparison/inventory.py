import fnmatch
import hashlib
import re
import subprocess

from .common import canonical_bytes, digest

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


# llm-cc's built-in rules (src/rules.cc) as fnmatch patterns, where `*`
# crosses `/` and a leading `**/` needs a slash, so each directory pattern
# also appears anchored at the top level. A rules file keeps the default of
# every key it omits, exactly as llm-cc applies it.
DEFAULT_RULES = {
    "exclude": [
        pattern
        for directory in (
            ".hg", ".svn", "target", "node_modules", ".gradle", ".venv",
            "__pycache__", ".tox", ".nox", ".mypy_cache", ".pytest_cache",
            ".ruff_cache", "vendor", "third_party", "build", "build-out",
            ".nuget", "dist", "deps", "_build", "cmake-build-debug",
            "cmake-build-release", "bazel-*",
        )
        for pattern in (directory + "/*", "*/" + directory + "/*")
    ]
    + ["out/*", "bin/*.cs", "*/bin/*.cs", "obj/*.cs", "*/obj/*.cs"],
    "tests": [
        pattern
        for directory in ("test", "tests", "testdata", "fixtures", "fuzz", "fuzzers")
        for pattern in (directory + "/*", "*/" + directory + "/*")
    ]
    + ["*_test.*", "test_*.*", "*/test_*.*"],
    "tooling": ["tools/*", "examples/*", "example/*", "scripts/*", "benchmarks/*"],
}


def with_defaults(rules):
    """`rules` with every omitted exclude, tests or tooling list defaulted."""
    return {**DEFAULT_RULES, **rules}


def validate_rules(rules):
    """Reject classification rules that a repository could use to hide work."""
    if not isinstance(rules, dict):
        raise ValueError("classification rules must be a JSON object")
    allowed = {"exclude", "tests", "tooling", "extensions", "paths"}
    unknown = set(rules) - allowed
    if unknown:
        raise ValueError(
            "unsupported classification rule keys: " + ", ".join(sorted(unknown))
        )
    total = 0
    for name in ("exclude", "tests", "tooling"):
        if name not in rules:
            continue
        patterns = rules[name]
        if not isinstance(patterns, list):
            raise ValueError("classification rule %s must be a list of globs" % name)
        total += len(patterns)
        for pattern in patterns:
            if not isinstance(pattern, str) or not pattern or len(pattern) > 256:
                raise ValueError(
                    "classification rule %s needs non-empty globs of at most 256 characters"
                    % name
                )
    paths = rules.get("paths", [])
    languages = set(LANGUAGES.values())
    if not isinstance(paths, list):
        raise ValueError("classification rule paths must be a list of objects")
    total += len(paths)
    for override in paths:
        if (
            not isinstance(override, dict)
            or set(override) != {"pattern", "language"}
            or not isinstance(override["pattern"], str)
            or not override["pattern"]
            or len(override["pattern"]) > 256
        ):
            raise ValueError(
                "classification rule paths needs {pattern, language} objects with "
                "non-empty globs of at most 256 characters"
            )
        if override["language"] not in languages:
            raise ValueError(
                "classification rule path %s names an unsupported language"
                % override["pattern"]
            )
    if total > 512:
        raise ValueError("classification rules use more than 512 glob patterns")
    extensions = rules.get("extensions", {})
    if not isinstance(extensions, dict):
        raise ValueError("classification rule extensions must be an object")
    for extension, language in extensions.items():
        if (
            not isinstance(extension, str)
            or not re.fullmatch(r"\.[A-Za-z0-9_+-]{1,16}", extension)
            or extension != extension.lower()
        ):
            raise ValueError(
                "classification rule extensions must map lowercase .<ext> names"
            )
        if language not in languages:
            raise ValueError(
                "classification rule extension %s names an unsupported language"
                % extension
            )
    if len(canonical_bytes(rules)) > 64 * 1024:
        raise ValueError("classification rules exceed 64 KiB")
    return rules


def read_tree_file(repo, revision, path):
    """Read one committed blob, returning None when the tree has no such path."""
    target = "%s:%s" % (revision, path)
    try:
        kind = _git(repo, ["cat-file", "-t", target]).strip()
    except GitError:
        return None
    if kind != b"blob":
        raise GitError("%s is not a regular file in %s" % (path, revision))
    return _git(repo, ["cat-file", "blob", target])


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


def resolve_language(path, rules):
    """First matching path override wins, then extensions, then the defaults.

    Overrides are matched against the full repository path, so a header that
    means C in one directory and C++ in another gets a different language, and
    therefore a different cache key, on each side.
    """
    for override in rules.get("paths", []):
        if fnmatch.fnmatchcase(path, override["pattern"]):
            return override["language"]
    return (LANGUAGES | rules.get("extensions", {})).get(_extension(path))


def _path(path_bytes):
    """A Git path as text, and whether its bytes were valid UTF-8.

    Undecodable bytes become `\\xHH`, as in llm-cc, so plans and reports stay
    valid JSON; such a path is recorded but never scored.
    """
    try:
        return path_bytes.decode("utf-8"), True
    except UnicodeDecodeError:
        return path_bytes.decode("utf-8", "backslashreplace"), False


def inventory(repo, revision, fingerprint, rules=None, max_file_bytes=65536):
    rules = validate_rules(rules or {})
    raw = _git(repo, ["ls-tree", "-rlz", "--full-tree", revision])
    records = []
    blob_ids = []
    for entry in raw.split(b"\0"):
        if not entry:
            continue
        metadata, path_bytes = entry.split(b"\t", 1)
        mode, kind, object_id, size_text = metadata.split()
        path, valid = _path(path_bytes)
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
        if kind == b"blob" and valid:
            record["language"] = resolve_language(path, rules)
        if mode == b"160000" or kind == b"commit":
            record["reason"] = "submodule"
        elif mode == b"120000":
            record["reason"] = "symlink"
        elif kind != b"blob" or not valid:
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
            return _path(p)[0]

        result.append(
            {
                "status": status,
                "old_path": None if status.startswith("A") else decode(old),
                "new_path": None if status.startswith("D") else decode(new),
            }
        )
    return result
