# Release Process for PGE Treemap

This document explains how to create and publish new releases for `pge_treemap`.

The repository is configured with a GitHub Actions workflow that automatically builds the release binary on Ubuntu 24.04 and attaches the `pge_treemap` executable as a downloadable asset directly onto the GitHub Release page.

---

## Method 1: Using the GitHub Web Interface (Recommended)

1. Navigate to the repository on GitHub: `https://github.com/plops/pge_treemap`
2. On the right sidebar, click **Releases** (or go directly to `https://github.com/plops/pge_treemap/releases`).
3. Click the **Draft a new release** button.
4. Fill in the release details:
   - **Choose a tag**: Type a new tag name using semantic versioning (e.g., `v1.0.0`) and click **Create new tag on publish**.
   - **Target**: Ensure `main` is selected.
   - **Release title**: Enter a title (e.g., `PGE Treemap v1.0.0`).
   - **Description**: Click **Generate release notes** or write release notes manually.
5. Click **Publish release**.

> **What happens next:** 
> - The `CMake Build PGE Treemap` GitHub Action will trigger immediately.
> - Once the job finishes (~30-40 seconds), refresh your release page.
> - The compiled binary (`pge_treemap`) will appear under the **Assets** section ready for download.

---

## Method 2: Using the GitHub CLI (`gh`)

If you have `gh` installed and authenticated on your local machine:

```bash
# Ensure main branch is up to date
git checkout main
git pull

# Create and publish the release with auto-generated release notes
gh release create v1.0.0 --title "v1.0.0" --generate-notes
