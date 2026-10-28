"""Build discovery: which httpd is under test, and what it can do.

One place answers that for both suites. ``resolve_build(config)`` finds the
httpd from, in order of precedence, ``--apxs``, ``--httpd`` (+ ``--conf``), or
the ``config.ini`` that the autoconf build generates for the modules/ suite,
probes it once per session, and returns an :class:`HttpdInfo`. The collection
time gating in :mod:`pyhttpd.gating` and the report header read it; the
classic framework builds its configuration on the same object.

The probing mirrors the discovery logic in Apache::TestConfigParse (perl-apache-test):
run ``httpd -v`` / ``-V`` / ``-l`` and parse the inherited install ``httpd.conf``
to learn the server version, MPM, compile-time ``-D`` defines, and the full set
of available modules (static + dynamically loaded).

The merged module set drives ``<IfModule>`` blocks, the module-name token
resolution in :mod:`apache_pytest.config`, and (eventually) the ``need_module``
test marks.
"""

from __future__ import annotations

import configparser
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

# httpd -V emits lines like:  -D APACHE_MPM_DIR="server/mpm/prefork"
_DEFINE_RE = re.compile(r"^\s*-D\s+([A-Z0-9_]+)(?:=(.*))?\s*$")
# httpd -l / module conf lines end in ".c";  -l indents them two spaces.
_STATIC_MOD_RE = re.compile(r"^\s*(\w+\.c)\s*$")
# LoadModule foo_module modules/mod_foo.so  (optionally commented with leading #)
_LOADMODULE_RE = re.compile(r"^\s*(#?)\s*LoadModule\s+(\S+)\s+(\S+)")
_VERSION_RE = re.compile(r"Apache/(\d+)\.(\d+)\.(\d+)")


@dataclass
class LoadDirective:
    """One ``LoadModule symbol file`` mapping from an install conf."""

    symbol: str  # e.g. "session_module"
    so: str  # path as written, e.g. "modules/mod_session.so"
    active: bool  # False if the line was commented out

    @property
    def cname(self) -> str:
        """Module source name, e.g. "mod_session.c" (used as the modules-set key)."""
        stem = Path(self.so).name
        for suffix in (".so", ".dll"):
            if stem.endswith(suffix):
                stem = stem[: -len(suffix)]
                break
        if stem.startswith("lib"):  # libphp.so -> mod_php.c
            stem = "mod_" + stem[3:]
        return f"{stem}.c"

    @property
    def is_mpm(self) -> bool:
        return self.symbol.startswith("mpm_") or "mpm_" in Path(self.so).name


@dataclass
class HttpdInfo:
    """Everything probed from an httpd binary + its inherited config."""

    httpd: Path
    version: tuple[int, int, int]
    mpm: str
    defines: dict[str, str | bool] = field(default_factory=dict)
    # Set of module source-file names, e.g. {"core.c", "mod_proxy.c", ...}.
    # This is the union of statically-compiled modules and modules available
    # via LoadModule in the inherited httpd.conf (active OR commented) — matching
    # Apache::Test's $self->{modules} hash keyed on "<name>.c". Membership here
    # means "this module exists and can be loaded", which is what drives the
    # <IfModule> blocks and the need_module marks.
    modules: set[str] = field(default_factory=set)
    # Every LoadModule directive discovered in the inherited conf, with absolute
    # .so paths. Re-emitted into the generated httpd.conf so the test server
    # actually loads them (the generated conf does not Include the install conf).
    load_directives: list[LoadDirective] = field(default_factory=list)
    # TAKE1 directives inherited from the install conf (wanted_config in
    # TestConfigParse.pm:34): ServerAdmin, TypesConfig, DocumentRoot, ServerRoot.
    # Keyed by directive name (as written), value is the single argument.
    inherited: dict[str, str] = field(default_factory=dict)
    # Where the build was found: "apxs", "httpd" or "config.ini".
    source: str = ""
    # Install prefix, if known.
    prefix: Path | None = None

    @property
    def version_str(self) -> str:
        return ".".join(str(p) for p in self.version)

    def has_module(self, name: str) -> bool:
        """True if module ``name`` is available.

        Accepts every form the Perl tests use: a bare name (``status``), a
        ``mod_``-prefixed name (``mod_status``), or any of those with a ``.c``/
        ``.so`` suffix. Modules are stored as source names (``mod_status.c``),
        so a bare name is also tried with the ``mod_`` prefix -- matching
        Apache::Test's need_module/have_module, which accept ``need_module 'status'``.
        """
        stem = name
        for suffix in (".c", ".so"):
            if stem.endswith(suffix):
                stem = stem[: -len(suffix)]
                break
        candidates = {f"{stem}.c"}
        if not stem.startswith("mod_"):
            candidates.add(f"mod_{stem}.c")
        return bool(candidates & self.modules)

    def version_at_least(self, major: int, minor: int, patch: int = 0) -> bool:
        return self.version >= (major, minor, patch)


def _run(cmd: list[str]) -> str:
    """Run a probe command, returning combined stdout (httpd prints to stdout)."""
    proc = subprocess.run(  # noqa: S603 - trusted, caller-provided httpd path
        cmd,
        capture_output=True,
        text=True,
        check=False,
    )
    # httpd -V can exit non-zero if it wants a config; we only need the banner.
    return proc.stdout + proc.stderr


def probe_version(httpd: Path) -> tuple[int, int, int]:
    out = _run([str(httpd), "-v"])
    m = _VERSION_RE.search(out)
    if not m:
        raise RuntimeError(f"could not parse version from `{httpd} -v`:\n{out}")
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)))


def probe_defines_and_mpm(httpd: Path) -> tuple[dict[str, str | bool], str]:
    """Parse ``httpd -V`` for ``-D`` defines, server MPM, and module magic info."""
    out = _run([str(httpd), "-V"])
    defines: dict[str, str | bool] = {}
    mpm = ""
    for line in out.splitlines():
        m = _DEFINE_RE.match(line)
        if m:
            key, val = m.group(1), m.group(2)
            defines[key] = val.strip('"') if val else True
            continue
        # "Server MPM:     event"  (2.4+/2.5)
        sm = re.match(r"\s*Server MPM:\s*(.+)", line, re.IGNORECASE)
        if sm:
            mpm = sm.group(1).strip().lower()
    if not mpm:
        # 2.0-era fallback: derive from APACHE_MPM_DIR=server/mpm/<name>
        mpm_dir = defines.get("APACHE_MPM_DIR")
        if isinstance(mpm_dir, str):
            mpm = Path(mpm_dir).name.lower()
    return defines, mpm


def probe_static_modules(httpd: Path) -> set[str]:
    """Modules compiled statically into the binary (``httpd -l``)."""
    out = _run([str(httpd), "-l"])
    mods: set[str] = set()
    for line in out.splitlines():
        m = _STATIC_MOD_RE.match(line)
        if m:
            mods.add(m.group(1))
    return mods


def parse_loadmodules(conf: Path, server_root: Path) -> list[LoadDirective]:
    """Parse ``LoadModule`` lines (active and commented) from an install httpd.conf.

    The suite is built with ``--enable-load-all-modules`` upstream; locally the
    stock install conf lists most modules commented out. We capture both so the
    generated config can re-emit them and so module-presence checks succeed.
    Relative ``.so`` paths are resolved against ``server_root`` (the install
    prefix, ``apxs -q PREFIX``). ``Include`` directives are not followed — the
    stock conf lists modules directly, sufficient for the local build target.
    """
    directives: list[LoadDirective] = []
    if not conf.is_file():
        return directives
    for raw in conf.read_text(errors="replace").splitlines():
        m = _LOADMODULE_RE.match(raw)
        if not m:
            continue
        commented, symbol, so = m.group(1), m.group(2), m.group(3)
        so_path = Path(so)
        if not so_path.is_absolute():
            so_path = server_root / so_path
        directives.append(
            LoadDirective(symbol=symbol, so=str(so_path), active=(commented == ""))
        )
    return directives


# TAKE1 directives inherited from the install conf (wanted_config TAKE1).
# ServerRoot is intentionally NOT applied (spec_apply maps it to a no-op so the
# test serverroot wins); we still record it for completeness but config.py
# ignores it.
_INHERIT_TAKE1 = ("ServerAdmin", "TypesConfig", "DocumentRoot", "ServerRoot")
_TAKE1_RE = re.compile(
    r"^\s*(" + "|".join(_INHERIT_TAKE1) + r")\s+(.+?)\s*$"
)


def parse_inherited_take1(conf: Path) -> dict[str, str]:
    """Parse last-wins TAKE1 directives from an install conf (wanted_config)."""
    found: dict[str, str] = {}
    if not conf.is_file():
        return found
    for raw in conf.read_text(errors="replace").splitlines():
        if raw.lstrip().startswith("#"):
            continue
        m = _TAKE1_RE.match(raw)
        if m:
            found[m.group(1)] = m.group(2).strip().strip('"')
    return found


def probe(
    httpd: Path,
    inherited_conf: Path | None = None,
    server_root: Path | None = None,
) -> HttpdInfo:
    """Full probe of an httpd binary and (optionally) its install config.

    :param httpd: path to the httpd executable.
    :param inherited_conf: install ``httpd.conf`` to scan for LoadModule lines.
        Typically ``<prefix>/conf/httpd.conf`` derived from ``apxs -q SYSCONFDIR``.
    :param server_root: install prefix used to resolve relative ``.so`` paths
        (``apxs -q PREFIX``). Defaults to the parent of ``inherited_conf``'s dir.
    """
    version = probe_version(httpd)
    defines, mpm = probe_defines_and_mpm(httpd)
    modules = probe_static_modules(httpd)
    load_directives: list[LoadDirective] = []
    inherited: dict[str, str] = {}
    if inherited_conf is not None:
        root = server_root or inherited_conf.parent.parent
        inherited = parse_inherited_take1(inherited_conf)
        all_directives = parse_loadmodules(inherited_conf, root)
        # Inherit only ACTIVE LoadModule lines, matching Apache::Test's
        # inherit_load_module (which parses the live config, not commented
        # lines). A build intended for the suite activates all modules
        # (--enable-load-all-modules); locally the install conf is activated to
        # match. Commented lines are ignored entirely.
        load_directives = [d for d in all_directives if d.active]
        for d in load_directives:
            if Path(d.so).exists():
                modules.add(d.cname)
    return HttpdInfo(
        httpd=httpd,
        version=version,
        mpm=mpm,
        defines=defines,
        modules=modules,
        load_directives=load_directives,
        inherited=inherited,
    )


# --------------------------------------------------------------------------- #
# Finding the build under test
# --------------------------------------------------------------------------- #


class NoServerError(Exception):
    """Raised when no httpd could be located from the options or config.ini.

    A plain exception, not pytest.fail, so the collection time callers can
    catch it without turning into an INTERNALERROR.
    """


def apxs_query(apxs: Path, var: str) -> str:
    proc = subprocess.run(  # noqa: S603 - trusted path
        ["perl", str(apxs), "-q", var], capture_output=True, text=True, check=True
    )
    return proc.stdout.strip()


def resolve_paths(
    config,
) -> tuple[Path, Path | None, Path | None, Path | None, list[str]]:
    """Resolve (httpd, apxs, inherited_conf, install_prefix, defines) from options.

    Raises :class:`NoServerError` if neither --httpd nor --apxs is given.
    """
    apxs_opt = config.getoption("--apxs")
    httpd_opt = config.getoption("--httpd")
    defines = [d for d in config.getoption("--defines").split() if d]

    apxs = Path(apxs_opt) if apxs_opt else None
    inherited_conf: Path | None = None
    install_prefix: Path | None = None
    if apxs is not None:
        sbindir = Path(apxs_query(apxs, "SBINDIR"))
        sysconfdir = Path(apxs_query(apxs, "SYSCONFDIR"))
        install_prefix = Path(apxs_query(apxs, "PREFIX"))
        inherited_conf = sysconfdir / "httpd.conf"
        if httpd_opt is None:
            httpd_opt = str(sbindir / "httpd")

    conf_opt = config.getoption("--conf")
    prefix_opt = config.getoption("--prefix")
    if conf_opt is not None and inherited_conf is None:
        inherited_conf = Path(conf_opt)
    if prefix_opt is not None:
        install_prefix = Path(prefix_opt)
    elif inherited_conf is not None and install_prefix is None:
        install_prefix = inherited_conf.parent.parent

    if httpd_opt is None:
        raise NoServerError("must pass --httpd or --apxs")
    return Path(httpd_opt), apxs, inherited_conf, install_prefix, defines


def config_ini_path() -> Path:
    """The autoconf generated config.ini of the modules/ suite."""
    return Path(os.getenv("PYHTTPD_CONFIG", Path(__file__).resolve().parent / "config.ini"))


def _probe_config_ini(path: Path) -> HttpdInfo:
    """Probe the httpd described by a generated config.ini.

    The ini names the build prefix and the shared modules that were built, so
    those count as available whether or not the install's httpd.conf loads
    them (the modules/ suite writes its own LoadModule lines).
    """
    ini = configparser.ConfigParser(interpolation=configparser.ExtendedInterpolation())
    ini.read(path)
    name = ini.get("httpd", "name", fallback="httpd")
    if sys.platform == "win32":
        httpd = Path(ini.get("global", "bindir")) / f"{name}.exe"
    else:
        httpd = Path(ini.get("global", "sbindir", fallback=ini.get("global", "bindir"))) / name
    if not httpd.is_file():
        raise NoServerError(f"{path}: no httpd binary at {httpd}")
    info = probe(httpd)
    for mod in (ini.get("httpd", "dso_modules", fallback="").split()
                + ini.get("httpd", "mpm_modules", fallback="").split()):
        info.modules.add(f"{mod if mod.startswith('mod_') else 'mod_' + mod}.c")
    info.source = "config.ini"
    info.prefix = Path(ini.get("global", "prefix"))
    return info


_BUILD_KEY = None


def resolve_build(config) -> HttpdInfo | None:
    """Probe the httpd under test, once per session; None if there is none.

    Precedence: ``--apxs``, then ``--httpd`` (+ ``--conf``), then the
    config.ini generated by the build. A None result is not an error here:
    callers that need a server fail on their own with a clearer message.
    """
    global _BUILD_KEY  # noqa: PLW0603 - the stash key is created lazily
    import pytest

    if _BUILD_KEY is None:
        _BUILD_KEY = pytest.StashKey[HttpdInfo | None]()
    if _BUILD_KEY in config.stash:
        return config.stash[_BUILD_KEY]

    info: HttpdInfo | None = None
    try:
        httpd, apxs, inherited_conf, prefix, _defines = resolve_paths(config)
        info = probe(httpd, inherited_conf, prefix)
        info.source = "apxs" if apxs is not None else "httpd"
        info.prefix = prefix
    except Exception:
        ini = config_ini_path()
        if ini.is_file():
            try:
                info = _probe_config_ini(ini)
            except Exception:
                info = None
    config.stash[_BUILD_KEY] = info
    return info
