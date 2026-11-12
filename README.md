# French Input — floating French accent palette

A tiny plain C / Win32 tool: a single exe, no dependencies, about 1–3 MB of memory.
Type French special characters in any program (Notepad, Word, browsers…) without adding a keyboard layout.

## Usage

- **Click a button**: the character is inserted at the current caret (the window never takes focus)
- **Aa**: uppercase lock; or hold Shift while clicking
- **Drag**: grab the header or any gap between buttons; the position is remembered
- **×**: exit

## Hotkeys (Win+Alt+letter, press repeatedly to cycle)

| Hotkey | Repeated presses give |
|---|---|
| Win+Alt+E | é → è → ê → ë |
| Win+Alt+A | à → â → æ |
| Win+Alt+C | ç |
| Win+Alt+I | î → ï → ÿ |
| Win+Alt+O | ô → œ |
| Win+Alt+U | ù → û → ü |
| Win+Alt+Q | « → » |
| add Shift | uppercase (É, À, Ç, Œ…) |
| Win+Alt+H | show / hide the window |

Pressing again after more than 1.5 seconds starts over. Keep Win+Alt held and tap E repeatedly to cycle.

ÿ is in the I group because Win+Alt+Y is often taken by other software (Win+Alt+F likewise, which is why show/hide uses H).

## Building

Requires Visual Studio (or Build Tools) with the "Desktop development with C++" workload.

```
build.bat          detects the host architecture (x64 host → x64, ARM host → arm64)
build.bat x64
build.bat arm64
```

Produces `french_input_x64.exe` or `french_input_arm64.exe`.

On an ARM machine, install the **MSVC ARM64 build tools** component.

## Notes

- A Chinese (or other) IME in composition mode may intercept the input; switch it to English mode while typing French.
- Programs running as administrator do not receive keystrokes sent by a normal program (a Windows security rule). To type into them, run this tool as administrator too.
- Start with Windows: press Win+R, type `shell:startup`, and put a shortcut to the exe there.
- If a hotkey is taken by another program, the header shows "Hotkey in use: Win+Alt+…".
- With Smart App Control enabled, a freshly built exe may occasionally be blocked; rebuilding usually produces a file that is allowed.
