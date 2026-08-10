Set WshShell = CreateObject("WScript.Shell")
' Runs the Chicken AC server silently in the background without popping up a command window.
WshShell.Run "python ""C:\Users\Lenovo\.gemini\antigravity\scratch\climate-controller\server\app.py"" --port DEMO --web-port 8000", 0, False
