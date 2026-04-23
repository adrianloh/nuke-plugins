#!/usr/bin/env python3
"""
build.py - Build nuke-plugins on GitHub Actions and drop the compiled
           binaries into the repo's <plugin>/Release/<platform>/ folders
           so they can be committed and pushed for the team to download.

Usage:
  python build.py                  # List buildable plugins and exit
  python build.py BaseSixFour      # Build one plugin
  python build.py all              # Build every plugin sequentially

  python build.py BaseSixFour --download
                                   # Skip building; just download the
                                   # artifacts from the most recent
                                   # successful run for that plugin

What is a "buildable plugin"?
  Any top-level directory in the repo (sibling to this script) that
  contains a CMakeLists.txt. The 'scripts/' and '.github/' folders are
  ignored. Repo layout is the single source of truth for what's
  buildable - no registry to maintain.

Output:
  Compiled binaries land in:
    <repo>/<Plugin>/Release/windows/<Plugin>.dll
    <repo>/<Plugin>/Release/macos_arm64/<Plugin>.dylib

  The script does not commit or push - that's your job:
    git add */Release/
    git commit -m "Update built binaries"
    git push

Token (in priority order):
  1. --token <TOKEN>
  2. $GITHUB_TOKEN environment variable
  3. A folder named 'github_pat_<rest of token>' next to this script

Prerequisites:
  pip install requests
"""

from __future__ import annotations

import argparse
import io
import os
import sys
import time
import zipfile
from pathlib import Path
from typing import Optional

try:
    import requests
except ImportError:
    sys.exit("ERROR: 'requests' library not found. Run: pip install requests")


# --- Configuration -----------------------------------------------------------

REPO_NAME         = "nuke-plugins"
WORKFLOW_FILE     = "release.yml"   # The orchestrator in .github/workflows/
DEFAULT_BRANCH    = "main"

# Top-level directories to skip when discovering plugins.
NON_PLUGIN_DIRS = {".github", ".git", "scripts", "__pycache__"}

# Map an artifact's platform suffix to the in-repo Release subdirectory.
# Artifact names are '<plugin>-<artifact_suffix>' (defined in build-plugin.yml).
PLATFORM_DIR_MAP = {
    "windows-x86_64": "windows",
    "macos-arm64":    "macos_arm64",
}

# Poll settings
POLL_INTERVAL_SEC = 10
POLL_TIMEOUT_MIN  = 30


# --- Helpers -----------------------------------------------------------------

def log(msg: str) -> None:
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def repo_root() -> Path:
    """The directory the script lives in, which is the repo root."""
    return Path(__file__).resolve().parent


def discover_plugins() -> list[str]:
    """Return sorted list of buildable plugin names. A plugin is any top-level
    directory in the repo (excluding hidden + 'scripts/') that contains a
    CMakeLists.txt."""
    root = repo_root()
    plugins = []
    for entry in sorted(root.iterdir()):
        if not entry.is_dir():
            continue
        if entry.name in NON_PLUGIN_DIRS:
            continue
        if entry.name.startswith("."):
            continue
        if (entry / "CMakeLists.txt").is_file():
            plugins.append(entry.name)
    return plugins


def api_headers(token: str) -> dict:
    return {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }


def verify_token_and_repo(token: str, username: str) -> None:
    log(f"Checking that {username}/{REPO_NAME} exists and the token works...")
    r = requests.get(
        f"https://api.github.com/repos/{username}/{REPO_NAME}",
        headers=api_headers(token),
        timeout=30,
    )
    if r.status_code == 404:
        sys.exit(
            f"ERROR: Can't find https://github.com/{username}/{REPO_NAME}\n"
            f"       Check --username, or create the repo first."
        )
    if r.status_code == 401:
        sys.exit("ERROR: Token is invalid or expired. Generate a new one.")
    r.raise_for_status()
    log("  Repo looks good.")


# --- Trigger + poll ----------------------------------------------------------

def trigger_workflow(token: str, username: str, plugin: str,
                     branch: str) -> float:
    """Dispatch the workflow and return the unix timestamp we captured
    just before the API call."""
    log(f"Triggering '{WORKFLOW_FILE}' on branch '{branch}' for plugin '{plugin}'...")
    url = (f"https://api.github.com/repos/{username}/{REPO_NAME}"
           f"/actions/workflows/{WORKFLOW_FILE}/dispatches")
    trigger_time = time.time()  # capture BEFORE the request
    r = requests.post(
        url,
        headers=api_headers(token),
        json={"ref": branch, "inputs": {"plugin": plugin}},
        timeout=30,
    )
    if r.status_code == 204:
        log("  Workflow dispatched.")
        return trigger_time
    if r.status_code == 404:
        sys.exit(
            f"ERROR: Workflow '{WORKFLOW_FILE}' not found on branch '{branch}'.\n"
            f"       Check: https://github.com/{username}/{REPO_NAME}/actions"
        )
    if r.status_code == 422:
        sys.exit(
            f"ERROR: Workflow dispatch rejected (422).\n"
            f"       Response: {r.text}"
        )
    r.raise_for_status()
    return trigger_time


def wait_for_run(token: str, username: str, trigger_time: float,
                 branch: str) -> dict:
    """Wait for a workflow run created strictly AFTER trigger_time.
    Returns the run dict on success. On failure, dumps logs and exits."""
    log("Waiting for the new workflow run to appear...")

    runs_url = (f"https://api.github.com/repos/{username}/{REPO_NAME}"
                f"/actions/workflows/{WORKFLOW_FILE}/runs")

    run_id: Optional[int] = None
    for attempt in range(60):  # up to ~2 minutes to appear
        r = requests.get(
            runs_url,
            headers=api_headers(token),
            params={"branch": branch, "event": "workflow_dispatch", "per_page": 5},
            timeout=30,
        )
        r.raise_for_status()
        runs = r.json().get("workflow_runs", [])
        fresh = []
        for run in runs:
            created_at = run.get("created_at", "")
            try:
                from datetime import datetime, timezone
                ts = datetime.strptime(created_at, "%Y-%m-%dT%H:%M:%SZ") \
                    .replace(tzinfo=timezone.utc).timestamp()
            except Exception:
                continue
            if ts > trigger_time:
                fresh.append((ts, run))
        if fresh:
            fresh.sort(key=lambda x: x[0])  # oldest fresh first
            run = fresh[0][1]
            run_id = run["id"]
            log(f"  Found run #{run['run_number']} (id {run_id})")
            log(f"  Live URL: {run['html_url']}")
            break
        if attempt == 0:
            log("  Not yet registered; waiting...")
        time.sleep(2)
    if run_id is None:
        sys.exit("ERROR: Fresh workflow run did not appear within 120 seconds.")

    run_url = f"https://api.github.com/repos/{username}/{REPO_NAME}/actions/runs/{run_id}"
    jobs_url = f"{run_url}/jobs"
    started = time.time()
    last_status = None

    seen_step_states: dict = {}

    timeout = POLL_TIMEOUT_MIN * 60
    while True:
        elapsed = int(time.time() - started)
        if elapsed > timeout:
            sys.exit(f"ERROR: Build did not finish within {POLL_TIMEOUT_MIN} minutes. "
                     f"Check the live URL.")

        r = requests.get(run_url, headers=api_headers(token), timeout=30)
        r.raise_for_status()
        run = r.json()
        status = run.get("status")
        conclusion = run.get("conclusion")

        if status != last_status:
            mins = elapsed // 60
            secs = elapsed % 60
            log(f"  [{mins:>2}m{secs:02d}s] status={status}"
                + (f" conclusion={conclusion}" if conclusion else ""))
            last_status = status

        jr = requests.get(jobs_url, headers=api_headers(token), timeout=30)
        if jr.ok:
            for job in jr.json().get("jobs", []):
                job_name = job.get("name", "?")
                for step in job.get("steps") or []:
                    step_name = step.get("name", "?")
                    step_status = step.get("status")
                    step_conc = step.get("conclusion")
                    key = (job_name, step_name)
                    state_key = (step_status, step_conc)
                    if seen_step_states.get(key) == state_key:
                        continue
                    seen_step_states[key] = state_key
                    if step_status == "in_progress":
                        mins = elapsed // 60
                        log(f"  [{mins:>2}m]   > {job_name} :: {step_name}")
                    elif step_status == "completed":
                        if step_conc in ("skipped", "cancelled"):
                            continue
                        icon = "x" if step_conc != "success" else "+"
                        mins = elapsed // 60
                        log(f"  [{mins:>2}m]   {icon} {job_name} :: {step_name} ({step_conc})")

        if status == "completed":
            if conclusion == "success":
                log(f"Build finished successfully in {elapsed}s.")
                return run
            else:
                report_failure(token, username, run, jobs_url)
                sys.exit(1)

        time.sleep(POLL_INTERVAL_SEC)


def report_failure(token: str, username: str, run: dict, jobs_url: str) -> None:
    """On workflow failure, print everything useful for debugging and
    auto-download log files for any failed job(s) so they can be shared."""
    run_id = run["id"]
    html_url = run["html_url"]

    print("")
    print("=" * 70)
    print(f"  BUILD FAILED (run #{run['run_number']}, conclusion: {run['conclusion']})")
    print("=" * 70)
    print(f"  Live URL:  {html_url}")
    print("")

    jr = requests.get(jobs_url, headers=api_headers(token), timeout=30)
    failed_jobs = []
    if jr.ok:
        jobs = jr.json().get("jobs", [])
        print(f"  Jobs:")
        for job in jobs:
            name = job.get("name", "?")
            conc = job.get("conclusion") or job.get("status")
            print(f"    [{conc:>9}]  {name}")
            if job.get("conclusion") not in (None, "success", "skipped"):
                failed_jobs.append(job)
                for step in job.get("steps") or []:
                    if step.get("conclusion") not in (None, "success", "skipped"):
                        print(f"                first failing step: "
                              f"\"{step.get('name')}\" "
                              f"({step.get('conclusion')})")
                        break

    downloaded: list[Path] = []
    errors: list[str] = []
    out_dir = Path.cwd()
    for job in failed_jobs:
        job_id = job["id"]
        safe = "".join(c if c.isalnum() or c in "-_" else "_"
                       for c in job.get("name", f"job-{job_id}"))
        out_path = out_dir / f"failed-run-{run['run_number']}-{safe}.log"
        try:
            log_url = (f"https://api.github.com/repos/{username}/{REPO_NAME}"
                       f"/actions/jobs/{job_id}/logs")
            lr = requests.get(log_url, headers=api_headers(token),
                              allow_redirects=True, stream=True, timeout=60)
            lr.raise_for_status()
            with open(out_path, "wb") as f:
                for block in lr.iter_content(chunk_size=64 * 1024):
                    if block:
                        f.write(block)
            downloaded.append(out_path)
        except Exception as e:
            errors.append(f"{safe}: {e}")

    zip_path = out_dir / f"failed-run-{run['run_number']}-all-logs.zip"
    try:
        logs_url = (f"https://api.github.com/repos/{username}/{REPO_NAME}"
                    f"/actions/runs/{run_id}/logs")
        zr = requests.get(logs_url, headers=api_headers(token),
                          allow_redirects=True, stream=True, timeout=120)
        zr.raise_for_status()
        with open(zip_path, "wb") as f:
            for block in zr.iter_content(chunk_size=64 * 1024):
                if block:
                    f.write(block)
        downloaded.append(zip_path)
    except Exception as e:
        errors.append(f"full-run-logs.zip: {e}")

    print("")
    if downloaded:
        print("  Logs downloaded to current directory:")
        for p in downloaded:
            size_kb = p.stat().st_size / 1024
            print(f"    {p.name}  ({size_kb:,.0f} KB)")
    if errors:
        print("")
        print("  Could not download these logs (API may be rate-limited):")
        for e in errors:
            print(f"    - {e}")
        print("")
        print("  Browser fallback - download logs from:")
        print(f"    {html_url}")
    print("")
    print("  Share the .log or .zip file for debugging.")
    print("=" * 70)


# --- Artifacts ---------------------------------------------------------------

def list_artifacts(token: str, username: str, run_id: int) -> list[dict]:
    url = (f"https://api.github.com/repos/{username}/{REPO_NAME}"
           f"/actions/runs/{run_id}/artifacts")
    r = requests.get(url, headers=api_headers(token),
                     params={"per_page": 100}, timeout=30)
    r.raise_for_status()
    return r.json().get("artifacts", [])


def artifact_to_release_subdir(artifact_name: str, plugin: str) -> Optional[str]:
    """Given an artifact name like 'BaseSixFour-windows-x86_64', return the
    in-repo Release subdir name ('windows', 'macos_arm64', etc.). Returns
    None if the artifact name doesn't fit the expected pattern."""
    prefix = f"{plugin}-"
    if not artifact_name.startswith(prefix):
        return None
    suffix = artifact_name[len(prefix):]
    return PLATFORM_DIR_MAP.get(suffix)


def download_artifact_to_release(token: str, username: str,
                                 artifact: dict, plugin: str) -> list[Path]:
    """Download an artifact zip from GitHub and extract its contents to
    <repo>/<plugin>/Release/<platform>/. Returns the list of extracted files."""
    name = artifact["name"]
    size = artifact.get("size_in_bytes", 0)

    sub = artifact_to_release_subdir(name, plugin)
    if sub is None:
        log(f"WARNING: artifact '{name}' has unexpected suffix; skipping.")
        return []

    out_dir = repo_root() / plugin / "Release" / sub

    url = (f"https://api.github.com/repos/{username}/{REPO_NAME}"
           f"/actions/artifacts/{artifact['id']}/zip")

    log(f"Downloading '{name}' ({size / 1024:.0f} KB) -> {out_dir.relative_to(repo_root())}/")
    r = requests.get(url, headers=api_headers(token),
                     stream=True, allow_redirects=True, timeout=120)
    r.raise_for_status()
    data = io.BytesIO()
    for block in r.iter_content(chunk_size=64 * 1024):
        if block:
            data.write(block)
    data.seek(0)

    extracted = []
    out_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(data) as zf:
        for member in zf.namelist():
            if member.endswith("/"):
                continue
            target = out_dir / Path(member).name
            with zf.open(member) as src, open(target, "wb") as dst:
                dst.write(src.read())
            extracted.append(target)
            log(f"  Extracted: {target.name}")
    return extracted


def find_latest_successful_run(token: str, username: str, plugin: str,
                               branch: str) -> dict:
    """Find the most recent successful workflow run whose artifacts
    match the requested plugin. Used by --download mode."""
    runs_url = (f"https://api.github.com/repos/{username}/{REPO_NAME}"
                f"/actions/workflows/{WORKFLOW_FILE}/runs")
    r = requests.get(
        runs_url,
        headers=api_headers(token),
        params={"branch": branch, "status": "success", "per_page": 20},
        timeout=30,
    )
    r.raise_for_status()
    runs = r.json().get("workflow_runs", [])
    if not runs:
        sys.exit(
            f"ERROR: No successful workflow runs found on branch '{branch}'.\n"
            f"       Trigger one first: rerun without --download."
        )

    for run in runs:
        artifacts = list_artifacts(token, username, run["id"])
        for a in artifacts:
            if a["name"].startswith(f"{plugin}-") and not a.get("expired", False):
                log(f"Latest successful run for {plugin}: #{run['run_number']} (id {run['id']})")
                log(f"  Completed: {run.get('updated_at', '?')}")
                log(f"  URL: {run['html_url']}")
                return run

    sys.exit(
        f"ERROR: No successful runs with artifacts for plugin '{plugin}' found.\n"
        f"       Trigger a build first: rerun without --download."
    )


# --- One-plugin orchestration ------------------------------------------------

def build_one(plugin: str, *, token: str, username: str, branch: str,
              download_only: bool) -> list[Path]:
    """Trigger (or skip-to-download) one plugin and drop its artifacts into
    the in-repo Release dirs. Returns the list of extracted file paths."""
    print("")
    print("#" * 70)
    print(f"#  {plugin}")
    print("#" * 70)

    if download_only:
        run = find_latest_successful_run(token, username, plugin, branch)
    else:
        trigger_time = trigger_workflow(token, username, plugin, branch)
        run = wait_for_run(token, username, trigger_time, branch)

    artifacts = list_artifacts(token, username, run["id"])
    plugin_artifacts = [a for a in artifacts
                        if a["name"].startswith(f"{plugin}-")]

    if not plugin_artifacts:
        sys.exit(f"ERROR: Run #{run['run_number']} has no artifacts for "
                 f"plugin '{plugin}'.")

    log(f"Found {len(plugin_artifacts)} artifact(s) for {plugin}:")
    for a in plugin_artifacts:
        flag = " (expired)" if a.get("expired") else ""
        log(f"  - {a['name']}{flag}")

    extracted: list[Path] = []
    for a in plugin_artifacts:
        if a.get("expired"):
            log(f"Skipping expired artifact: {a['name']}")
            continue
        extracted.extend(
            download_artifact_to_release(token, username, a, plugin)
        )

    return extracted


# --- Main --------------------------------------------------------------------

def find_token_in_script_dir() -> Optional[str]:
    """Look for a folder named like 'github_pat_...' next to this script."""
    matches = [p.name for p in repo_root().iterdir()
               if p.is_dir() and p.name.startswith("github_pat_")]
    if not matches:
        return None
    if len(matches) > 1:
        log(f"WARNING: Multiple github_pat_* folders found; using {matches[0]}")
    return matches[0]


def print_plugins_and_exit(plugins: list[str], code: int = 0) -> None:
    if not plugins:
        print("No buildable plugins found in the repo.")
        print("(A buildable plugin is any top-level directory with a CMakeLists.txt.)")
    else:
        print("Buildable plugins:")
        for p in plugins:
            print(f"  - {p}")
        print("")
        print("Usage:")
        print("  python build.py <plugin>          # build one")
        print("  python build.py all               # build every plugin sequentially")
        print("  python build.py <plugin> --download")
        print("                                     # skip the build; just download last successful")
    sys.exit(code)


def main() -> None:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("target", nargs="?", default=None,
                   help="Plugin name to build, or 'all'. Omit to list buildable plugins.")
    p.add_argument("--username", default="adrianloh",
                   help="GitHub username (owner of the nuke-plugins repo). "
                        "Default: adrianloh.")
    p.add_argument("--branch", default=DEFAULT_BRANCH,
                   help=f"Branch to build from. Default: {DEFAULT_BRANCH}.")
    p.add_argument("--token", default=os.environ.get("GITHUB_TOKEN"),
                   help="GitHub Personal Access Token. Defaults to $GITHUB_TOKEN, "
                        "or to the name of a 'github_pat_*' folder next to this script.")
    p.add_argument("--download", action="store_true",
                   help="Skip triggering a build; just download artifacts from "
                        "the most recent successful run for the chosen plugin(s).")
    args = p.parse_args()

    plugins = discover_plugins()

    # No-arg invocation: show available plugins and exit
    if args.target is None:
        print_plugins_and_exit(plugins)

    # Resolve target -> list of plugins to build
    target_lower = args.target.lower()
    if target_lower == "all":
        if not plugins:
            sys.exit("ERROR: No buildable plugins found in the repo.")
        to_build = plugins
    else:
        # Case-insensitive name match against discovered plugins
        match = next((p for p in plugins if p.lower() == target_lower), None)
        if match is None:
            print(f"ERROR: '{args.target}' is not a buildable plugin.")
            print("")
            print_plugins_and_exit(plugins, code=2)
        to_build = [match]

    # Token resolution
    if not args.token:
        args.token = find_token_in_script_dir()
        if args.token:
            log(f"Using token from folder name next to script.")

    if not args.token:
        sys.exit(
            "ERROR: No token provided.\n"
            "       Pass --token <TOKEN>, set GITHUB_TOKEN, or create an empty\n"
            "       folder named 'github_pat_<rest of token>' next to this script."
        )

    verify_token_and_repo(args.token, args.username)

    # -- Build each in turn --------------------------------------------------
    summary: list[tuple[str, list[Path]]] = []
    for plugin in to_build:
        try:
            extracted = build_one(
                plugin,
                token=args.token,
                username=args.username,
                branch=args.branch,
                download_only=args.download,
            )
            summary.append((plugin, extracted))
        except SystemExit:
            # build_one already printed the failure context. In 'all' mode
            # we keep going so the rest of the plugins still try.
            summary.append((plugin, []))
            if len(to_build) == 1:
                raise

    # -- Final summary -------------------------------------------------------
    print("")
    print("=" * 70)
    print("  Summary")
    print("=" * 70)
    any_success = False
    for plugin, extracted in summary:
        if not extracted:
            print(f"  {plugin}:  FAILED")
            continue
        any_success = True
        print(f"  {plugin}:")
        for f in extracted:
            try:
                rel = f.relative_to(repo_root())
            except ValueError:
                rel = f
            size_kb = f.stat().st_size / 1024
            print(f"    {rel}  ({size_kb:,.0f} KB)")
    print("=" * 70)
    if any_success:
        print("")
        print("Next: review the changes and commit:")
        print("  git status")
        print("  git add */Release/")
        print("  git commit -m 'Update built binaries'")
        print("  git push")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log("Aborted by user.")
        sys.exit(130)
