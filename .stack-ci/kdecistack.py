"""The stack of kde-ci-local: our forks that other projects build against.

A stack file lists the members (fork, branch, what each needs).  Each member is built once,
by its own pipeline job with its dependencies from KDE's registry and the members it needs
laid over them, and its staged install is kept under

    $WORK/stack/NAME/members/MEMBER-KEY/prefix

KEY is a hash of the member's source (its top-level tree without the CI-only paths, see
source_id), the keys of the members it needs and the id of the
job image, so a member is rebuilt when its commit, a member it needs or the image moves,
and only then.  KDE's own packages are not part of the key: the registry changes under us
(see KDE-CI.md).

A project job gets a composed prefix, the members' prefixes hard-linked into one folder
(NAME/compose/CKEY), which stack-run-ci-build lays over _install after KDE's packages.
"""
import hashlib
import json
import os
import shutil
import subprocess

import yaml


def load(path):
    with open(path) as f:
        stack = yaml.safe_load(f)
    root = os.environ.get("KDE_CI_STACK_REPOS", stack.get("repos", "."))
    stack["root"] = root
    stack["order"] = []
    seen = set()

    def visit(name, trail=()):
        if name in seen:
            return
        if name in trail:
            raise SystemExit(f"stack {stack['name']}: {name} needs itself")
        for dep in stack["members"][name].get("needs", []):
            visit(dep, trail + (name,))
        seen.add(name)
        stack["order"].append(name)

    for name in stack["members"]:
        visit(name)
    return stack


def git(repo, *args):
    return subprocess.check_output(["git", "-C", repo, *args], text=True).strip()


# What a fork-only "CI only, not for upstream" commit touches.  The member is built by KDE's
# run-ci-build.py, which reads neither, so a change to them must not move the key.
CI_ONLY = (".stack-ci", ".gitlab-ci.yml")


def source_id(repo, rev):
    """Hash of the top-level tree of rev without the CI-only paths."""
    h = hashlib.sha1()
    for line in git(repo, "ls-tree", rev).splitlines():
        meta, name = line.split("\t", 1)
        if name not in CI_ONLY:
            h.update(f"{meta}\t{name}\n".encode())
    return h.hexdigest()


def key_of(name, commit, image_id, dep_keys):
    """commit is the member's source_id."""
    h = hashlib.sha1()
    h.update(f"{name}\n{commit}\n{image_id}\n".encode())
    for dep in sorted(dep_keys):
        h.update(f"{dep} {dep_keys[dep]}\n".encode())
    return h.hexdigest()[:12]


def resolve(stack, image_id):
    """Add commit, key and directory to every member."""
    out = {}
    for name in stack["order"]:
        m = dict(stack["members"][name])
        m["name"] = name
        m["package"] = name
        m["path"] = m["repo"] if os.path.isabs(m["repo"]) else os.path.join(stack["root"], m["repo"])
        m["commit"] = git(m["path"], "rev-parse", m["branch"] + "^{commit}")
        m["source"] = source_id(m["path"], m["commit"])
        m["key"] = key_of(name, m["source"], image_id, {d: out[d]["key"] for d in m.get("needs", [])})
        out[name] = m
    return out


def home(work, stack):
    return os.path.join(work, "stack", stack["name"])


def member_dir(work, stack, m):
    return os.path.join(home(work, stack), "members", f"{m['name']}-{m['key']}")


def is_built(work, stack, m):
    return os.path.exists(os.path.join(member_dir(work, stack, m), "build.json"))


def closure(members, name):
    """name and every member it needs, directly or not."""
    out, todo = set(), [name]
    while todo:
        n = todo.pop()
        if n not in out:
            out.add(n)
            todo += members[n].get("needs", [])
    return out


def dependents(members, name):
    """The members that need name, directly or not (not name itself)."""
    return {n for n in members if n != name and name in closure(members, n)}


def compose(work, stack, members, names):
    """Hard-link the prefixes of names into one folder keyed by theirs; -> (folder, manifest)."""
    chosen = {n: members[n] for n in stack["order"] if n in names}
    key = hashlib.sha1("".join(f"{n}-{m['key']}\n" for n, m in chosen.items()).encode()).hexdigest()[:12]
    dest = os.path.join(home(work, stack), "compose", key)
    manifest = {"stack": stack["name"], "key": key,
                "members": {n: {k: m[k] for k in ("package", "branch", "commit", "key")} for n, m in chosen.items()}}
    if not os.path.exists(os.path.join(dest, "stack.json")):
        tmp = dest + ".tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        files = os.path.join(tmp, "files")
        os.makedirs(files)
        for n, m in chosen.items():
            src = os.path.join(member_dir(work, stack, m), "prefix")
            for root, dirs, fs in os.walk(src):
                rel = os.path.relpath(root, src)
                out = files if rel == "." else os.path.join(files, rel)
                os.makedirs(out, exist_ok=True)
                for f in fs:
                    s, d = os.path.join(root, f), os.path.join(out, f)
                    if os.path.lexists(d):
                        os.remove(d)
                    if os.path.islink(s):
                        os.symlink(os.readlink(s), d)
                    else:
                        os.link(s, d)
        with open(os.path.join(tmp, "stack.json"), "w") as f:
            json.dump(manifest, f, indent=1)
        shutil.rmtree(dest, ignore_errors=True)
        os.rename(tmp, dest)
    return dest, manifest


def harvest(work, stack, m, staged, seconds):
    """Keep the staged install of a member that has just been built."""
    dest = member_dir(work, stack, m)
    tmp = dest + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    prefix = os.path.join(tmp, "prefix")
    shutil.copytree(staged, prefix, symlinks=True, copy_function=lambda s, d, **kw: os.link(s, d))
    count = sum(len(fs) for _, _, fs in os.walk(prefix))
    size = int(subprocess.check_output(["du", "-sk", prefix], text=True).split()[0]) * 1024
    with open(os.path.join(tmp, "build.json"), "w") as f:
        json.dump({k: m[k] for k in ("name", "branch", "commit", "key")} | {
            "needs": m.get("needs", []), "seconds": seconds, "files": count, "bytes": size}, f, indent=1)
    shutil.rmtree(dest, ignore_errors=True)
    os.rename(tmp, dest)
    return count, size


def free_gb(path):
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize / 2**30


def disk_blocked(need_gb):
    """-> a message when need_gb more on /K3D would leave it under its 20 GB floor (or / under 12 GB), else None."""
    for path, floor in (("/K3D", 20), ("/", 12)):
        if os.path.exists(path) and free_gb(path) - (need_gb if path == "/K3D" else 0) < floor:
            return f"{path}: {free_gb(path):.1f} GB free, floor {floor} GB (build needs about {need_gb} GB)"
    return None
