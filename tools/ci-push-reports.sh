#!/bin/sh
set -eu

force_push=
while getopts "f" opt; do
    case "$opt" in
        f) force_push=1 ;;
        *) echo "Usage: $0 [-f]" >&2; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

branch=$(git rev-parse --abbrev-ref HEAD)
if [ "$branch" = "main" ]; then
    echo "Error: refusing to run on main branch" >&2
    exit 1
fi

if [ -n "$force_push" ]; then
    echo "Force-pushing branch '$branch' to trigger CI..."
    git push origin "$branch" --force-with-lease
else
    echo "Pushing branch '$branch' to trigger CI..."
    git push origin "$branch"
fi

echo "Waiting for CI run to appear..."
sleep 5

run_id=$(gh run list --branch "$branch" --limit 1 --json databaseId --jq '.[0].databaseId')
if [ -z "$run_id" ]; then
    echo "Error: could not find CI run for branch '$branch'" >&2
    exit 1
fi

echo "Watching CI run $run_id..."
gh run watch --exit-status "$run_id" || {
    rc=$?
    echo "Warning: CI run $run_id failed or was cancelled (exit $rc)" >&2
}

echo "Downloading artifacts..."
downloaded=
m_files="bench/bench_report_darwin.md test/tcc_test_arm64.md test_report_arm64.md"
w_files="bench/bench_report_mingw.md test_report_mingw.md test/tcc_test_mingw.md"
l_files="test/tcc_test_linux.md test/torture_report_linux.log test_report_linux.md"
dirs="linux-reports macos-reports windows-reports"
rm -rf linux-reports macos-reports windows-reports
if gh run download "$run_id" -D . 2>/dev/null; then
    downloaded=yes
else
    echo "  -> no artifacts found (skipping)" >&2
fi

if [ -z "$downloaded" ]; then
    echo "No artifacts were downloaded, nothing to do."
    exit 0
fi
pwd="$(pwd)"
for d in $dirs; do
    cd "$d" && rsync -av . ..
    cd "$pwd" || exit 1
done

echo "Staging downloaded report files..."
# shellcheck disable=SC2086
git add $m_files $w_files $l_files 2>/dev/null || true
rm -rf macos-reports windows-reports linux-reports || true

if git diff --cached --quiet; then
    echo "No report files changed, nothing to commit."
    exit 0
fi

echo "Amending commit (pre-commit hooks will reformat files)..."
if ! git commit --amend --no-edit; then
    echo "Note: commit amended but hooks may have modified files." >&2
fi

if git diff --quiet; then
    echo "No hook changes to stage."
else
    echo "Staging formatting changes from hooks..."
    git add -u
    echo "Amending again to include hook changes..."
    git commit --amend --no-edit
fi

echo "Force-pushing amended commit..."
git push origin "$branch" --force-with-lease

echo "Done."
