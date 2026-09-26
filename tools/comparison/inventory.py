import functools
import hashlib
import re
import string
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


# llm-cc's built-in rules (src/rules.cc). A rules file keeps the default of
# every list it omits, exactly as llm-cc applies it.
DEFAULT_RULES = {
    "exclude": [
        "**/.hg/**", "**/.svn/**", "**/target/**", "**/node_modules/**",
        "**/.gradle/**", "**/.venv/**", "**/__pycache__/**", "**/.tox/**",
        "**/.nox/**", "**/.mypy_cache/**", "**/.pytest_cache/**",
        "**/.ruff_cache/**", "**/vendor/**", "**/third_party/**", "**/build/**",
        "**/build-out/**", "**/.nuget/**", "**/dist/**", "**/deps/**",
        "**/_build/**", "**/cmake-build-debug/**", "**/cmake-build-release/**",
        "**/bazel-*/**", "out/**", "**/bin/**/*.cs", "**/obj/**/*.cs",
    ],
    "tests": [
        "**/test/**", "**/tests/**", "**/testdata/**", "**/fixtures/**",
        "**/fuzz/**", "**/fuzzers/**", "**/*_test.*", "**/test_*.*",
    ],
    "tooling": ["tools/**", "examples/**", "example/**", "scripts/**", "benchmarks/**"],
}


def _patterns(rules, name):
    return rules[name] if name in rules else DEFAULT_RULES[name]


def _check_glob(pattern):
    """Reject what llm-cc's Glob::Compile rejects, with its reasons."""

    def invalid(reason):
        raise ValueError("invalid glob %r: %s" % (pattern, reason))

    if pattern.startswith("/") or pattern.endswith("/"):
        invalid("patterns are relative to the repository root and match files")
    for segment in pattern.split("/"):
        if not segment:
            invalid("empty path segment")
        if segment in (".", ".."):
            invalid("'.' and '..' segments never match a repository path")
        index = 0
        while index < len(segment):
            if segment[index] == "\\":
                index += 1
                if index >= len(segment):
                    invalid("trailing backslash")
            elif segment[index] == "[":
                end = index + 1
                if end < len(segment) and segment[end] in "!^":
                    end += 1
                if end < len(segment) and segment[end] == "]":
                    end += 1
                while end < len(segment) and segment[end] != "]":
                    end += 2 if segment[end] == "\\" else 1
                if end >= len(segment):
                    invalid("unterminated '['")
                index = end
            index += 1


def _match_class(pattern, index, value):
    """Match the set starting at pattern[index] == "["; return (hit, end)."""
    index += 1
    negated = index < len(pattern) and pattern[index] in "!^"
    index += negated
    matched = False
    first = True
    while index < len(pattern) and (first or pattern[index] != "]"):
        first = False
        if pattern[index] == "\\":
            index += 1
        low = high = pattern[index]
        index += 1
        if index + 1 < len(pattern) and pattern[index] == "-" and pattern[index + 1] != "]":
            index += 1
            if pattern[index] == "\\":
                index += 1
            high = pattern[index]
            index += 1
        # A reversed range matches nothing.
        matched = matched or low <= value <= high
    return matched != negated, index + 1


def _match_segment(pattern, text):
    """One segment, as llm-cc matches it: the last `*` absorbs characters."""
    position = offset = 0
    star = None
    star_offset = 0
    while offset < len(text):
        if position < len(pattern) and pattern[position] == "*":
            while position < len(pattern) and pattern[position] == "*":
                position += 1
            star, star_offset = position, offset
            continue
        if position < len(pattern):
            token = pattern[position]
            if token == "?":
                hit, following = True, position + 1
            elif token == "[":
                hit, following = _match_class(pattern, position, text[offset])
            else:
                literal = position + 1 if token == "\\" else position
                hit, following = pattern[literal] == text[offset], literal + 1
            if hit:
                position, offset = following, offset + 1
                continue
        if star is None:
            return False
        star_offset += 1
        position, offset = star, star_offset
    while position < len(pattern) and pattern[position] == "*":
        position += 1
    return position == len(pattern)


@functools.lru_cache(maxsize=4096)
def _glob(pattern):
    """llm-cc's segment-aware glob (src/glob.cc) as its segments.

    Patterns are anchored at the repository root; `*` and `?` never cross
    `/`; a whole-segment `**` matches zero or more directories, and a
    trailing `/**` one or more path segments. Matching is linear in the
    pattern and path, never a backtracking regular expression.
    """
    segments = pattern.split("/")
    if segments[-1] == "**":
        segments[-1:] = ["*", "**"]
    return tuple(segments)


def _glob_matches(pattern, path):
    segments = _glob(pattern)
    parts = [part for part in path.split("/") if part]
    position = index = 0
    star = None
    star_index = 0
    while index < len(parts):
        if position < len(segments) and segments[position] == "**":
            position += 1
            star, star_index = position, index
            continue
        if position < len(segments) and _match_segment(segments[position], parts[index]):
            position += 1
            index += 1
            continue
        if star is None:
            return False
        star_index += 1
        position, index = star, star_index
    while position < len(segments) and segments[position] == "**":
        position += 1
    return position == len(segments)


def _matches(path, patterns):
    return any(_glob_matches(pattern, path) for pattern in patterns)


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
            _check_glob(pattern)
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
        _check_glob(override["pattern"])
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
        # Absent unless a parent exists as something other than a directory,
        # which llm-cc reports rather than reading as no rules.
        parts = path.split("/")
        for depth in range(1, len(parts)):
            parent = "%s:%s" % (revision, "/".join(parts[:depth]))
            try:
                parent_kind = _git(repo, ["cat-file", "-t", parent]).strip()
            except GitError:
                return None
            if parent_kind != b"tree":
                raise GitError(
                    "%s is not a directory in %s" % ("/".join(parts[:depth]), revision)
                )
        return None
    listing = _git(repo, ["ls-tree", "-z", revision, "--", path]).split(b"\0")[0]
    if kind != b"blob" or not listing.startswith((b"100644 ", b"100755 ")):
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


# llm-cc folds only ASCII letters in extensions.
_ASCII_LOWER = str.maketrans(string.ascii_uppercase, string.ascii_lowercase)


def _extension(path):
    name = path.rsplit("/", 1)[-1]
    dot = name.rfind(".")
    return name[dot:].translate(_ASCII_LOWER) if dot >= 0 else ""


def _always_excluded(path):
    """Git metadata and llm-cc caches, which no rules file can include."""
    return any(part in (".git", ".llm-cc-cache") for part in path.split("/"))


def _in_virtual_environment(path_bytes, environments):
    """Whether the root or a directory above `path_bytes` holds a pyvenv.cfg.

    Both are raw Git path bytes: an escaped spelling could name a different
    directory.
    """
    if b"" in environments:
        return True
    directories = path_bytes.split(b"/")[:-1]
    return any(
        b"/".join(directories[: depth + 1]) in environments
        for depth in range(len(directories))
    )


def _classify(path, rules):
    if _matches(path, _patterns(rules, "tests")):
        return "tests"
    if _matches(path, _patterns(rules, "tooling")):
        return "tooling"
    return "runtime"


def resolve_language(path, rules):
    """First matching path override wins, then extensions, then the defaults.

    Overrides are matched against the full repository path, so a header that
    means C in one directory and C++ in another gets a different language, and
    therefore a different cache key, on each side.
    """
    for override in rules.get("paths", []):
        if _glob_matches(override["pattern"], path):
            return override["language"]
    return (LANGUAGES | rules.get("extensions", {})).get(_extension(path))


def _path(path_bytes):
    """A Git path as rules match it, and as a plan records it.

    Recorded paths spell `\\` as `\\\\` and each undecodable byte as `\\xHH`,
    as llm-cc does, so distinct paths stay distinct and plans valid JSON. A path
    that is not UTF-8 has no text to match and is never scored.
    """
    try:
        text = path_bytes.decode("utf-8")
    except UnicodeDecodeError:
        escaped = path_bytes.replace(b"\\", b"\\\\")
        return None, escaped.decode("utf-8", "backslashreplace")
    return text, text.replace("\\", "\\\\")


def inventory(repo, revision, fingerprint, rules=None, max_file_bytes=65536):
    rules = validate_rules(rules or {})
    raw = _git(repo, ["ls-tree", "-rlz", "--full-tree", revision])
    entries = [entry for entry in raw.split(b"\0") if entry]
    # llm-cc skips any directory holding a pyvenv.cfg, as it does locally.
    environments = set()
    for entry in entries:
        metadata, path_bytes = entry.split(b"\t", 1)
        if metadata.split()[0] in (b"100644", b"100755") and (
            path_bytes.rsplit(b"/", 1)[-1] == b"pyvenv.cfg"
        ):
            environments.add(path_bytes.rpartition(b"/")[0])
    records = []
    blob_ids = []
    for entry in entries:
        metadata, path_bytes = entry.split(b"\t", 1)
        mode, kind, object_id, size_text = metadata.split()
        text, path = _path(path_bytes)
        matched = path if text is None else text
        size = None if size_text == b"-" else int(size_text)
        record = {
            "path": path,
            "language": None,
            "category": _classify(matched, rules),
            "size": size,
            "content_sha256": None,
            "key": None,
            "scorable": False,
            "reason": None,
            "_object": object_id.decode("ascii"),
        }
        if kind == b"blob" and text is not None:
            record["language"] = resolve_language(text, rules)
        if mode == b"160000" or kind == b"commit":
            record["reason"] = "submodule"
        elif mode == b"120000":
            record["reason"] = "symlink"
        elif kind != b"blob" or text is None:
            record["reason"] = "unsupported"
        elif (
            _always_excluded(matched)
            or _in_virtual_environment(path_bytes, environments)
            or _matches(matched, _patterns(rules, "exclude"))
        ):
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
            return _path(p)[1]

        result.append(
            {
                "status": status,
                "old_path": None if status.startswith("A") else decode(old),
                "new_path": None if status.startswith("D") else decode(new),
            }
        )
    return result
