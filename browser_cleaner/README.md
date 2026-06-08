# Browser Cleaner

Windows-focused Python TUI app for cleaning browser history, search records,
saved login databases, cookies, cache, local storage, and session files.

Supported browsers:

- Google Chrome
- Microsoft Edge
- Mozilla Firefox
- Brave

## Run

Dry run first:

```powershell
python .\main.py
```

Delete matching browser data:

```powershell
python .\main.py --apply
```

Clean only one browser:

```powershell
python .\main.py --browser chrome --apply
python .\main.py --browser edge --apply
python .\main.py --browser firefox --apply
python .\main.py --browser brave --apply
```

Force-close supported browsers before cleaning:

```powershell
python .\main.py --apply --kill
```

The cleaner requires typing `CLEAN` before deletion unless `--yes` is also
passed. Without `--apply`, it only displays what would be removed.

## Structure

- `main.py`: executable entry point
- `browser_cleaner/models.py`: shared data models
- `browser_cleaner/config.py`: browser definitions and cleanup targets
- `browser_cleaner/profiles.py`: browser profile discovery
- `browser_cleaner/processes.py`: Windows process checks and optional browser closing
- `browser_cleaner/cleaning.py`: path deletion and dry-run behavior
- `browser_cleaner/tui.py`: terminal rendering
- `browser_cleaner/app.py`: workflow orchestration
