#!/usr/bin/env python3
"""
build_nuke_plugin.py - Build a nuke-plugins plugin on GitHub Actions and
                       download the compiled binaries to your Downloads folder.

What this does:
  1. Verifies your fork/repo exists and the token works.
  2. Dispatches the 'Build / Release' workflow for the plugin you specify.
  3. Polls the run until it finishes (typically 3-6 minutes).
  4. Downloads the Windows .dll and macOS .dylib to your Downloads folder.

Prerequisites:
  - The repo 'nuke-plugins' exists under your GitHub account.
  - You have a GitHub Personal Access Token with 'Actions: write' and
    'Contents: read' scopes on that repo.
  - pip install requests

Usage:
  # Build BaseSixFour (default plugin)
  python build_nuke_plugin.py

  # Build a specific plugin
  python build_nuke_plugin.py --plugin Loki

  # Just download the artifacts from the most recent successful run
  # (no new build triggered)
  python build_nuke_plugin.py --plugin BaseSixFour --download

  # Token resolution order:
  #   1. --token <TOKEN>
  #   2. $GITHUB_TOKEN environment variable
  #   3. A folder named 'github_pat_<...>' sitting next to this script
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
DEFAULT_PLUGIN    = "BaseSixFour"

# Poll settings
POLL_INTERVAL_SEC = 10
POLL_TIMEOUT_MIN  = 30


# --- Helpers -----------------------------------------------------------------

def log(msg: str) -> None:
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


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
    just before the API call. wait_for_run uses this to filter out stale
    runs from previous dispatches."""
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
            f"       Likely causes: plugin name not in the dropdown options, or\n"
            f"       the branch has no workflow file.\n"
            f"       Response: {r.text}"
        )
    r.raise_for_status()
    return trigger_time


def wait_for_run(token: str, username: str, trigger_time: float,
                 branch: str) -> dict:
    """Wait for a workflow run created strictly AFTER trigger_time.
    Returns the run dict once it has reached a terminal state (success
    or failure)."""
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

    # Track per-job step progress so we can print "Windows: Build started"
    # etc. as things happen.
    seen_step_states: dict = {}

    timeout = POLL_TIMEOUT_MIN * 60
    while True:
        elapsed = int(time.time() - started)
        if elapsed > timeout:
            sys.exit(f"ERROR: Build did not finish within {POLL_TIMEOUT_MIN} minutes. "
                     f"Check the live URL.")

        # --- Top-level run status -------------------------------------------
        r = requests.get(run_url, headers=api_headers(token), timeout=30)
        r.raise_for_status()
        run = r.json()
        status = run.get("status")       # queued | in_progress | completed
        conclusion = run.get("conclusion")  # success | failure | cancelled | ...

        if status != last_status:
            mins = elapsed // 60
            secs = elapsed % 60
            log(f"  [{mins:>2}m{secs:02d}s] status={status}"
                + (f" conclusion={conclusion}" if conclusion else ""))
            last_status = status

        # --- Per-job step progress ------------------------------------------
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
                        icon = "x" if step_conc != "success" else "+"
                        if step_conc in ("skipped", "cancelled"):
                            continue  # too noisy
                        mins = elapsed // 60
                        log(f"  [{mins:>2}m]   {icon} {job_name} :: {step_name} ({step_conc})")

        # --- Terminal? ------------------------------------------------------
        if status == "completed":
            if conclusion == "success":
                log(f"Build finished successfully in {elapsed}s.")
            else:
                sys.exit(f"ERROR: Build finished with conclusion '{conclusion}'.\n"
                         f"       Check: {run['html_url']}")
            return run

        time.sleep(POLL_INTERVAL_SEC)


# --- Artifacts ---------------------------------------------------------------

def list_artifacts(token: str, username: str, run_id: int) -> list[dict]:
    url = (f"https://api.github.com/repos/{username}/{REPO_NAME}"
           f"/actions/runs/{run_id}/artifacts")
    r = requests.get(url, headers=api_headers(token),
                     params={"per_page": 100}, timeout=30)
    r.raise_for_status()
    return r.json().get("artifacts", [])


def download_artifact(token: str, username: str, artifact: dict,
                      out_dir: Path) -> list[Path]:
    """Download one artifact (a zip from GitHub) and extract its contents
    into out_dir. Returns the list of extracted file paths."""
    name = artifact["name"]
    size = artifact.get("size_in_bytes", 0)
    url = (f"https://api.github.com/repos/{username}/{REPO_NAME}"
           f"/actions/artifacts/{artifact['id']}/zip")

    log(f"Downloading '{name}' ({size / 1024:.0f} KB)...")
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
            # Skip directory entries
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
            f"       Trigger one first (run without --download)."
        )

    # Walk runs newest-first. A run "matches" the plugin if it has an
    # artifact named '<plugin>-*'. This correctly skips runs for other
    # plugins when asking for one specific plugin.
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


# --- Main --------------------------------------------------------------------

def default_downloads_dir() -> Path:
    if os.name == "nt":
        return Path(os.environ.get("USERPROFILE", Path.home())) / "Downloads"
    return Path.home() / "Downloads"


def find_token_in_script_dir() -> Optional[str]:
    """Look for a folder named like 'github_pat_...' next to this script,
    and return its name. This lets you stash the token as a folder name
    so you don't have to set $GITHUB_TOKEN every shell session."""
    script_dir = Path(__file__).resolve().parent
    matches = [p.name for p in script_dir.iterdir()
               if p.is_dir() and p.name.startswith("github_pat_")]
    if not matches:
        return None
    if len(matches) > 1:
        log(f"WARNING: Multiple github_pat_* folders found next to script; "
            f"using {matches[0]}")
    return matches[0]


def main() -> None:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--username", default="adrianloh",
                   help="GitHub username (owner of the nuke-plugins repo). "
                        "Default: adrianloh.")
    p.add_argument("--plugin", default=DEFAULT_PLUGIN,
                   help=f"Plugin directory name to build. Default: {DEFAULT_PLUGIN}.")
    p.add_argument("--branch", default=DEFAULT_BRANCH,
                   help=f"Branch to build from. Default: {DEFAULT_BRANCH}.")
    p.add_argument("--token", default=os.environ.get("GITHUB_TOKEN"),
                   help="GitHub Personal Access Token. Defaults to $GITHUB_TOKEN, "
                        "or to the name of a 'github_pat_*' folder next to this script.")
    p.add_argument("--download-dir", type=Path, default=None,
                   help="Where to save the built plugin binaries. Default: "
                        "<Downloads>/<plugin>/")
    p.add_argument("--download", action="store_true",
                   help="Skip triggering a build; just download artifacts from "
                        "the most recent successful run for this plugin.")
    args = p.parse_args()

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

    # Output dir
    if args.download_dir is None:
        args.download_dir = default_downloads_dir() / args.plugin

    verify_token_and_repo(args.token, args.username)

    # -- --download mode: just grab the latest artifacts ----------------------
    if args.download:
        run = find_latest_successful_run(args.token, args.username,
                                         args.plugin, args.branch)
    else:
        # -- Normal mode: trigger + poll + download --------------------------
        trigger_time = trigger_workflow(args.token, args.username,
                                        args.plugin, args.branch)
        run = wait_for_run(args.token, args.username, trigger_time, args.branch)

    # -- Download artifacts ---------------------------------------------------
    artifacts = list_artifacts(args.token, args.username, run["id"])
    plugin_artifacts = [a for a in artifacts
                        if a["name"].startswith(f"{args.plugin}-")]

    if not plugin_artifacts:
        sys.exit(f"ERROR: Run #{run['run_number']} has no artifacts for "
                 f"plugin '{args.plugin}'.")

    log(f"Found {len(plugin_artifacts)} artifact(s) for {args.plugin}:")
    for a in plugin_artifacts:
        flag = " (expired)" if a.get("expired") else ""
        log(f"  - {a['name']}{flag}")

    all_extracted: list[Path] = []
    for a in plugin_artifacts:
        if a.get("expired"):
            log(f"Skipping expired artifact: {a['name']}")
            continue
        extracted = download_artifact(args.token, args.username, a,
                                      args.download_dir)
        all_extracted.extend(extracted)

    print("")
    print("=" * 60)
    print(f"  {args.plugin} build complete")
    print(f"  Output dir: {args.download_dir}")
    for f in all_extracted:
        print(f"    {f.name}  ({f.stat().st_size / 1024:.0f} KB)")
    print("=" * 60)
    print("")
    print("Install:")
    if os.name == "nt":
        print(f"  Copy the .dll to %USERPROFILE%\\.nuke\\")
    else:
        print(f"  Copy the .dylib to ~/.nuke/")
    print("  Restart Nuke.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log("Aborted by user.")
        sys.exit(130)
